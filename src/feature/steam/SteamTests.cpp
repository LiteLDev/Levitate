// Selftests of the steam unit (U18): `/create selftest "steam.*"`. Test area dx 1800..1899.

#include <algorithm>

#include "ll/api/coro/CoroTask.h"

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/steam/Steam.h"
#include "feature/steam/SteamInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::steam {

namespace {

using selftest::Context;
namespace fluids = common::fluids;

struct Area {
    Context& ctx;
    int      base;
    GlobalPos at(int dx, int dy = 0, int dz = 0) const {
        auto p = ctx.testPos(base + dx, dz);
        p.y += dy;
        return p;
    }
};

bool place(Context& ctx, GlobalPos const& p, char const* id, std::vector<std::pair<std::string, int>> const& ints = {},
           std::vector<std::pair<std::string, std::string>> const& strings = {}) {
    bool ok = ctx.setBlock(p, id, ints);
    if (ok && !strings.empty())
        if (auto* r = regionOf(p.dim)) setStates(*r, p, {}, strings);
    return ok;
}

void clear(GlobalPos const& p) {
    if (auto* r = regionOf(p.dim)) game::clearBlock(*r, game::blockPos(p));
}

std::string blockAt(GlobalPos const& p) {
    auto* r = regionOf(p.dim);
    return r ? idAt(*r, p) : std::string{};
}

int state(GlobalPos const& p, char const* name) {
    auto* r = regionOf(p.dim);
    if (!r) return -1;
    return intState(r->getBlock(game::blockPos(p)), name).value_or(-1);
}

ItemStack item(Context& ctx, std::string const& id, int count = 1) {
    auto s = common::items::make(id, count);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

int fill(GlobalPos const& p, std::string const& fluid, int amount, bool simulate = false) {
    auto* r = regionOf(p.dim);
    return r ? fluids::fill(*r, game::blockPos(p), -1, {fluid, amount}, simulate) : 0;
}

fluids::FluidStack drain(GlobalPos const& p, std::string const& fluid, int amount, bool simulate = false) {
    auto* r = regionOf(p.dim);
    return r ? fluids::drain(*r, game::blockPos(p), -1, {fluid, amount}, simulate) : fluids::FluidStack{};
}

/// A tank of width w and height h with its min corner at `a.at(x, y, z)`.
void tankBox(Context& ctx, Area const& a, int x, int y, int z, int w, int h, char const* id = kTank) {
    for (int dy = 0; dy < h; ++dy)
        for (int dx = 0; dx < w; ++dx)
            for (int dz = 0; dz < w; ++dz) place(ctx, a.at(x + dx, y + dy, z + dz), id);
}

std::string joined(std::vector<std::string> const& lines) {
    std::string s;
    for (auto const& l : lines) s += l + " | ";
    return s;
}

} // namespace

LV_REGISTER_SELFTEST("steam.registry", [](Context& ctx) {
    for (auto const* id : {kTank, kCreativeTank, kEngine, kWhistle, kExtension, kPoweredShaft, kTestHeater})
        ctx.check(game::blockWith(id) != nullptr, std::string(id) + " is in the behavior pack");
    auto const* spec = kinetics::specOf(kPoweredShaft);
    ctx.check(spec && spec->generator, "the powered shaft is a kinetic generator");
    ctx.check(kinetics::stressCapacityOf(kEngine) == 1024, "steam engine capacity 1024 su per rpm");
    auto p = tankParams(false);
    ctx.check(p.capacityPerBlock == 8000 && p.maxHeight == 32 && p.maxWidth == 3, "tank config: 8 buckets per block, 32 high, 3 wide");
});

LV_REGISTER_SELFTEST("steam.tank_forms", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1800};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    tankBox(ctx, a, 0, 1, 0, 2, 2);
    co_await ctx.ticks(2);
    auto t = tank(a.at(1, 2, 1));
    ctx.require(t.has_value(), "the top corner is a tank");
    ctx.check(t->width == 2 && t->height == 2, "a 2x2x2 tank formed: " + std::to_string(t->width) + "x" + std::to_string(t->height));
    ctx.check(t->controller == a.at(0, 1, 0), "its controller is the lowest corner");
    ctx.check(t->capacity == 64000, "capacity 8 blocks x 8000 mB: " + std::to_string(t->capacity));
    ctx.check(state(a.at(0, 1, 0), "levitate:bottom") == 1 && state(a.at(0, 1, 0), "levitate:top") == 0, "bottom part states");
    ctx.check(state(a.at(1, 2, 1), "levitate:top") == 1 && state(a.at(1, 2, 1), "levitate:bottom") == 0, "top part states");
    ctx.check(state(a.at(0, 1, 0), "levitate:shape") == 2 && state(a.at(1, 1, 1), "levitate:shape") == 5, "corner windows (nw / se)");
    // a third layer on top joins
    for (int x = 0; x < 2; ++x)
        for (int z = 0; z < 2; ++z) place(ctx, a.at(x, 3, z), kTank);
    co_await ctx.ticks(2);
    t = tank(a.at(0, 3, 0));
    ctx.check(t && t->height == 3 && t->controller == a.at(0, 1, 0), "a full layer on top makes it 3 high");
    // fluid tanks and creative tanks never connect
    place(ctx, a.at(4, 1, 0), kTank);
    place(ctx, a.at(4, 2, 0), kCreativeTank);
    co_await ctx.ticks(2);
    t = tank(a.at(4, 2, 0));
    ctx.check(t && t->height == 1 && t->creative, "a creative tank stays apart from a fluid tank");
});

LV_REGISTER_SELFTEST("steam.tank_fluid", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1806};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    place(ctx, a.at(0, 1), kTank);
    co_await ctx.ticks(2);
    ctx.check(fill(a.at(0, 1), "minecraft:water", 5000) == 5000, "accepts 5 buckets of water");
    ctx.check(fill(a.at(0, 1), "minecraft:lava", 1000) == 0, "refuses another fluid");
    ctx.check(fill(a.at(0, 1), "minecraft:water", 5000) == 3000, "fills up to 8000 mB");
    ctx.check(comparatorLevel(a.at(0, 1)) == 15, "comparator level 15 when full");
    auto got = drain(a.at(0, 1), "", 2500);
    ctx.check(got.fluid == "minecraft:water" && got.amount == 2500, "drains water");
    ctx.check(drain(a.at(0, 1), "minecraft:lava", 100).empty(), "nothing for a lava request");
    // a tank placed on top merges the contents
    place(ctx, a.at(0, 2), kTank);
    co_await ctx.ticks(2);
    auto t = tank(a.at(0, 2));
    ctx.check(t && t->height == 2 && t->amount == 5500 && t->capacity == 16000, "merged: 5500 / 16000 mB");
    ctx.check(fill(a.at(0, 2), "minecraft:water", 20000) == 10500, "the column takes 10500 more");
    auto lines = goggleLines(a.at(0, 1));
    ctx.check(lines.size() == 3 && lines[0].find("Fluid Container") != std::string::npos, "goggles: fluid container info: " + joined(lines));
});

LV_REGISTER_SELFTEST("steam.tank_split", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1812};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    for (int y = 1; y <= 3; ++y) place(ctx, a.at(0, y), kTank);
    co_await ctx.ticks(2);
    ctx.require(fill(a.at(0, 1), "minecraft:lava", 20000) == 20000, "fill the column with 20 buckets of lava");
    co_await ctx.ticks(1);
    ctx.check(state(a.at(0, 1), "levitate:light") == 12 && state(a.at(0, 3), "levitate:light") == 1, "lava glows up to its level");
    clear(a.at(0, 2));
    co_await ctx.ticks(2);
    auto bottom = tank(a.at(0, 1)), top = tank(a.at(0, 3));
    ctx.require(bottom && top, "both halves are tanks");
    ctx.check(bottom->height == 1 && bottom->amount == 8000, "the bottom keeps one block's worth: " + std::to_string(bottom->amount));
    ctx.check(top->height == 1 && top->amount == 8000 && top->fluid == "minecraft:lava", "the top gets the next 8000 mB");
});

LV_REGISTER_SELFTEST("steam.bucket", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1818};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    place(ctx, a.at(0, 1), kTank);
    co_await ctx.ticks(2);
    auto& p        = ctx.player();
    bool  creative = p.isCreative();
    p.setSelectedItem(item(ctx, "minecraft:water_bucket"));
    ctx.check(useItemOnTank(p, a.at(0, 1), p.getSelectedItem()), "a water bucket empties into the tank");
    auto t = tank(a.at(0, 1));
    ctx.check(t && t->amount == 1000 && t->fluid == "minecraft:water", "1000 mB of water");
    ctx.check(creative || common::items::idOf(p.getSelectedItem()) == "minecraft:bucket", "the bucket is empty now");
    p.setSelectedItem(item(ctx, "minecraft:bucket"));
    ctx.check(useItemOnTank(p, a.at(0, 1), p.getSelectedItem()), "an empty bucket takes it back");
    t = tank(a.at(0, 1));
    ctx.check(t && t->amount == 0, "the tank is empty");
    ctx.check(creative || common::items::idOf(p.getSelectedItem()) == "minecraft:water_bucket", "the bucket holds water");
    p.setSelectedItem(item(ctx, "minecraft:potion"));
    useItemOnTank(p, a.at(0, 1), p.getSelectedItem());
    t = tank(a.at(0, 1));
    ctx.check(t && t->amount == 250, "a water bottle adds 250 mB");
    p.setSelectedItem(common::items::empty());
});

LV_REGISTER_SELFTEST("steam.windows", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1824};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    tankBox(ctx, a, 0, 1, 0, 3, 1);
    co_await ctx.ticks(2);
    ctx.check(state(a.at(1, 1, 0), "levitate:shape") == 1 && state(a.at(0, 1, 0), "levitate:shape") == 0 && state(a.at(1, 1, 1), "levitate:shape") == 0,
              "3 wide: windows in the middle of each side only");
    ctx.check(toggleWindows(a.at(2, 1, 2)), "the wrench toggles windows");
    co_await ctx.ticks(1);
    ctx.check(state(a.at(1, 1, 0), "levitate:shape") == 0, "windows closed");
    toggleWindows(a.at(0, 1, 1));
    co_await ctx.ticks(1);
    ctx.check(state(a.at(0, 1, 1), "levitate:shape") == 1, "windows open again");
});

LV_REGISTER_SELFTEST("steam.multi_place", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1830};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    tankBox(ctx, a, 0, 1, 0, 2, 1);
    co_await ctx.ticks(2);
    for (int x = 0; x < 2; ++x)
        for (int z = 0; z < 2; ++z) ctx.cleanupBlock(a.at(x, 2, z));
    auto& p = ctx.player();
    p.setSelectedItem(item(ctx, kTank, 8));
    ctx.check(tryMultiPlace(p, a.at(1, 1, 1), 1, p.getSelectedItem()), "a tank used on the top of a 2x2 tank places a layer");
    co_await ctx.ticks(2);
    auto t = tank(a.at(0, 1, 0));
    ctx.check(t && t->height == 2 && t->width == 2, "the tank is 2 high now");
    ctx.check(p.isCreative() || common::items::countOf(p.getSelectedItem()) == 4, "four tanks used");
    p.setSelectedItem(common::items::empty());
});

LV_REGISTER_SELFTEST("steam.creative_tank", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1836};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    place(ctx, a.at(0, 1), kCreativeTank);
    co_await ctx.ticks(2);
    auto t = tank(a.at(0, 1));
    ctx.require(t.has_value(), "creative tank");
    setTankFluid(t->controller, "minecraft:lava", 1);
    t = tank(a.at(0, 1));
    ctx.check(t && t->amount == 8000, "a creative tank is always full");
    auto got = drain(a.at(0, 1), "", 3000);
    ctx.check(got.amount == 3000 && tank(a.at(0, 1))->amount == 8000, "draining doesn't empty it");
    ctx.check(fill(a.at(0, 1), "minecraft:water", 4000) == 4000 && tank(a.at(0, 1))->fluid == "minecraft:lava", "it voids what it is given");
});

LV_REGISTER_SELFTEST("steam.boiler_engine", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1842};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    // 3x3x2 tank (size 18: max level 4), two kindled burners under it
    for (int x = 0; x < 3; ++x)
        for (int z = 0; z < 3; ++z) place(ctx, a.at(x, 0, z), kTestHeater, {{"levitate:heat", (x == 1 && z < 2) ? 3 : 0}});
    tankBox(ctx, a, 0, 1, 0, 3, 2);
    // an engine on the east side and its shaft two blocks further east (axis z)
    place(ctx, a.at(3, 1, 1), kEngine, {}, {{"minecraft:block_face", "east"}});
    place(ctx, a.at(5, 1, 1), kPoweredShaft, {{"levitate:axis", 2}});
    co_await ctx.ticks(2);
    auto ctrl = a.at(0, 1, 0);
    auto t    = tank(ctrl);
    ctx.require(t && t->boiler, "the tank became a boiler");
    ctx.check(!t->window, "a boiler closes its windows");
    ctx.check(fill(ctrl, "minecraft:lava", 1000, true) == 0, "a boiler only takes water");
    // 40 mB of water per tick: water level 4
    for (int i = 0; i < 30; ++i) {
        fill(ctrl, "minecraft:water", 40);
        co_await ctx.ticks(1);
    }
    t = tank(ctrl);
    ctx.check(t && t->boilerLevel == 2, "boiler level 2 (heat 2, water 4, size 4): " + std::to_string(t ? t->boilerLevel : -1));
    auto shaft = a.at(5, 1, 1);
    ctx.check(kinetics::theoreticalSpeedAt(shaft) == -64, "one engine at full power: 64 rpm, got " + std::to_string(kinetics::theoreticalSpeedAt(shaft)));
    auto net = kinetics::networkAt(shaft);
    ctx.check(net && net->capacity == 16384, "16384 su, got " + std::to_string(net ? net->capacity : -1));
    auto lines = goggleLines(ctrl);
    ctx.check(!lines.empty() && lines[0].find("Lvl 2") != std::string::npos, "goggles: " + joined(lines));
    ctx.check(joined(lines).find("via 1 engine") != std::string::npos, "goggles show the engine");
    // the engine's value box turns it the other way
    setEngineCounterClockwise(a.at(3, 1, 1), true);
    for (int i = 0; i < 3; ++i) {
        fill(ctrl, "minecraft:water", 40);
        co_await ctx.ticks(1);
    }
    ctx.check(kinetics::theoreticalSpeedAt(shaft) == 64, "counter-clockwise: +64 rpm, got " + std::to_string(kinetics::theoreticalSpeedAt(shaft)));
    // without heat it idles
    for (int x = 0; x < 3; ++x)
        for (int z = 0; z < 3; ++z) place(ctx, a.at(x, 0, z), kTestHeater, {{"levitate:heat", 0}});
    for (int i = 0; i < 3; ++i) {
        fill(ctrl, "minecraft:water", 40);
        co_await ctx.ticks(1);
    }
    ctx.check(kinetics::theoreticalSpeedAt(shaft) == 0, "no heat: the shaft stops");
});

LV_REGISTER_SELFTEST("steam.passive_boiler", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1850};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    place(ctx, a.at(0, 0), kTestHeater, {{"levitate:heat", 1}}); // smouldering: passive heat
    for (int y = 1; y <= 4; ++y) place(ctx, a.at(0, y), kTank);
    place(ctx, a.at(0, 5), kEngine, {}, {{"minecraft:block_face", "up"}, {"minecraft:cardinal_direction", "south"}});
    place(ctx, a.at(0, 7), kPoweredShaft, {{"levitate:axis", 0}});
    co_await ctx.ticks(2);
    for (int i = 0; i < 20; ++i) {
        fill(a.at(0, 1), "minecraft:water", 10);
        co_await ctx.ticks(1);
    }
    auto t = tank(a.at(0, 1));
    ctx.check(t && t->passive, "a boiler on passive heat");
    auto shaft = a.at(0, 7);
    ctx.check(kinetics::theoreticalSpeedAt(shaft) == 16, "passive: 16 rpm, got " + std::to_string(kinetics::theoreticalSpeedAt(shaft)));
    auto net = kinetics::networkAt(shaft);
    ctx.check(net && net->capacity == 2048, "passive: 2048 su, got " + std::to_string(net ? net->capacity : -1));
    auto lines = goggleLines(a.at(0, 3));
    ctx.check(!lines.empty() && lines[0].find("Passive") != std::string::npos, "goggles: " + joined(lines));
});

LV_REGISTER_SELFTEST("steam.engine_support", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1858};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    place(ctx, a.at(0, 1), kEngine, {}, {{"minecraft:block_face", "up"}});
    place(ctx, a.at(2, 1), kTank);
    place(ctx, a.at(2, 2), kEngine, {}, {{"minecraft:block_face", "up"}});
    place(ctx, a.at(4, 1), kTank);
    place(ctx, a.at(4, 1, 1), kWhistle, {}, {{"minecraft:block_face", "south"}});
    co_await ctx.ticks(2);
    ctx.check(blockAt(a.at(0, 1)) != kEngine, "an engine needs a tank");
    ctx.check(blockAt(a.at(2, 2)) == kEngine, "an engine on a tank stays");
    ctx.check(blockAt(a.at(4, 1, 1)) == kWhistle, "a whistle on a tank's side stays");
    clear(a.at(2, 1));
    clear(a.at(4, 1));
    co_await ctx.ticks(2);
    ctx.check(blockAt(a.at(2, 2)) != kEngine, "the engine breaks with its tank");
    ctx.check(blockAt(a.at(4, 1, 1)) != kWhistle, "the whistle breaks with its tank");
});

LV_REGISTER_SELFTEST("steam.whistle", [](Context& ctx) -> ll::coro::CoroTask<> {
    Area a{ctx, 1864};
    ctx.require(co_await ctx.ensureLoaded(a.at(0)), "test area loaded");
    place(ctx, a.at(0, 0), kTestHeater, {{"levitate:heat", 3}});
    place(ctx, a.at(0, 1), kTank);
    place(ctx, a.at(0, 2), kWhistle, {}, {{"minecraft:block_face", "up"}});
    co_await ctx.ticks(2);
    for (int i = 3; i <= 6; ++i) ctx.cleanupBlock(a.at(0, i));
    auto& p = ctx.player();
    p.setSelectedItem(item(ctx, kWhistle));
    for (int i = 0; i < 3; ++i) useOnWhistle(p, a.at(0, 2), p.getSelectedItem(), false);
    co_await ctx.ticks(1);
    ctx.check(whistlePitch(a.at(0, 2)) == 3, "1.5 extensions: pitch 3, got " + std::to_string(whistlePitch(a.at(0, 2))));
    ctx.check(state(a.at(0, 3), "levitate:shape") == 2 && state(a.at(0, 4), "levitate:shape") == 0, "a connected full extension under a half one");
    ctx.check(common::items::countOf(p.getSelectedItem()) == 1, "the whistle in hand is not used up");
    p.setSelectedItem(common::items::empty());
    // powered by redstone on a heated boiler: it sounds
    place(ctx, a.at(1, 2), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    ctx.check(state(a.at(0, 2), "levitate:powered") == 1, "the whistle sees the redstone");
    ctx.check(whistleSounding(a.at(0, 2)), "it sounds");
    clear(a.at(1, 2));
    co_await ctx.ticks(4);
    ctx.check(!whistleSounding(a.at(0, 2)), "quiet without power");
    // the extensions go with the whistle
    clear(a.at(0, 2));
    co_await ctx.ticks(2);
    ctx.check(blockAt(a.at(0, 3)) != kExtension, "extensions vanish with their whistle");
});

} // namespace lv::feature::steam
