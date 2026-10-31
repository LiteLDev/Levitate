// Selftests of the item logistics rules aligned with Create in U55 (testPos dx 5550..5599).

#include <algorithm>

#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/funnels/FunnelsInternal.h"
#include "feature/goggles/GoggleInfo.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::core::GlobalPos;
using lv::selftest::Context;
namespace fu     = lv::feature::funnels;
namespace fd     = lv::feature::funnels::detail;
namespace cf     = lv::core::funnels;
namespace common = lv::feature::common;
namespace items  = lv::feature::common::items;

GlobalPos    at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
BlockPos     bp(GlobalPos const& p) { return BlockPos{p.x, p.y, p.z}; }
BlockSource& region(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

ItemStack item(Context& ctx, char const* id, int count = 1) {
    auto s = items::make(id, count);
    ctx.require(s.has_value(), std::string("item ") + id + " exists");
    return *s;
}

std::string idAt(Context& ctx, GlobalPos const& p) { return lv::game::blockIdAt(region(ctx), bp(p)); }

int stateAt(BlockSource& r, GlobalPos const& p, char const* name) {
    auto const* b = fd::loadedBlock(r, bp(p));
    return b ? fd::state(*b, name, -1) : -1;
}

int entitiesNear(BlockSource& r, GlobalPos const& pos, std::string const& id, float radius = 2.f) {
    int n = 0;
    for (auto* e : fd::itemsIn(r, Vec3{pos.x - radius, pos.y - radius, pos.z - radius}, Vec3{pos.x + 1 + radius, pos.y + 1 + radius, pos.z + 1 + radius})) {
        if (items::idOf(e->item()) != id) continue;
        n += items::countOf(e->item());
        e->remove();
    }
    return n;
}

} // namespace

LV_REGISTER_SELFTEST("funnels.chute_chain", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's ChuteItem.place: a click on a chute's side builds the diagonal chute one up
    auto base = at(ctx.testPos(5555), 0, 1, 2);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = region(ctx);
    ctx.setBlock(base, fu::kChute);
    co_await ctx.ticks(2);
    auto& p = ctx.spawnPlayer("lv_u55_chute");
    p.setPlayerGameType(GameType::Creative);
    co_await ctx.ticks(1);
    p.setSelectedItem(item(ctx, fu::kChute, 4));
    auto placed = fd::chuteItemPlace(p, r, bp(base), cf::kEast, false);
    co_await ctx.ticks(3);
    auto diag = at(base, 1, 1, 0);
    ctx.check(placed && *placed && idAt(ctx, diag) == fu::kChute, "a diagonal chute was placed one block up");
    ctx.check(stateAt(r, diag, fu::kFacing) == 4, "it leans towards the clicked chute (facing east)");
    ctx.check(!fd::chuteCanSurvive(r, bp(at(diag, 0, -1, 0))), "no chute under a diagonal one");
    ctx.check(fd::chuteCanSurvive(r, bp(at(base, 0, -1, 0))), "a chute under a straight one is fine");
    ctx.check(!fd::chuteItemPlace(p, r, bp(base), cf::kEast, true).has_value(), "sneaking places normally");
    for (auto q : {diag, base}) ctx.setBlock(q, "minecraft:air");
});

LV_REGISTER_SELFTEST("funnels.vault_layer", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's ItemVaultItem.tryMultiPlace: a vault on the end of a 2-wide vault fills the layer
    auto base = at(ctx.testPos(5560), 0, 1, 2);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = region(ctx);
    for (int y = 0; y < 2; ++y)
        for (int z = 0; z < 2; ++z) ctx.setBlock(at(base, 0, y, z), fu::kItemVault, {{fu::kAxis, 0}});
    co_await ctx.ticks(3);
    auto s = fd::vaultStructure(base);
    ctx.require(s && s->width == 2, "a 2x2 vault formed");
    auto& p = ctx.spawnPlayer("lv_u55_vault");
    p.setPlayerGameType(GameType::Survival);
    co_await ctx.ticks(1);
    p.setSelectedItem(item(ctx, fu::kItemVault, 8));
    auto first = at(base, 1, 0, 0);
    ctx.setBlock(first, fu::kItemVault, {{fu::kAxis, 0}});
    fd::tryMultiPlace(p, r, first, cf::kEast);
    co_await ctx.ticks(3);
    int placed = 0;
    for (int y = 0; y < 2; ++y)
        for (int z = 0; z < 2; ++z) placed += idAt(ctx, at(base, 1, y, z)) == fu::kItemVault ? 1 : 0;
    ctx.check(placed == 4, "the whole 2x2 layer was placed");
    ctx.check(items::countOf(p.getSelectedItem()) == 5, "three more vaults were used");
    auto s2 = fd::vaultStructure(base);
    ctx.check(s2 && s2->width == 2 && s2->length == 2, "a 2x2x2 vault formed");
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) ctx.setBlock(at(base, x, y, z), "minecraft:air");
});

LV_REGISTER_SELFTEST("funnels.chute_goggles", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's ChuteBlockEntity / BrassTunnelBlockEntity.addToGoggleTooltip
    auto base = at(ctx.testPos(5565), 0, 1, 2);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ctx.setBlock(base, fu::kChute);
    co_await ctx.ticks(2);
    auto rest = items::insert(region(ctx), bp(base), cf::kWest, item(ctx, "minecraft:apple", 3), false);
    ctx.check(items::isEmpty(rest), "the chute takes the stack");
    auto& p = ctx.spawnPlayer("lv_u55_goggles");
    co_await ctx.ticks(1);
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, item(ctx, "levitate:goggles"));
    auto ov    = lv::feature::goggles::overlayFor(p, base);
    auto has   = [&](std::string const& text) {
        return std::any_of(ov.lines.begin(), ov.lines.end(), [&](std::string const& l) { return l.find(text) != std::string::npos; });
    };
    ctx.check(has("Chute Information"), "the chute header");
    ctx.check(has("No attached fans"), "no fans");
    ctx.check(has("Items move Downward"), "the direction");
    ctx.check(has("Contains:"), "the contents");
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, ItemStack{});
    (void)items::extract(region(ctx), bp(base), cf::kDown, items::any(), 64, false);
    ctx.setBlock(base, "minecraft:air");
});

LV_REGISTER_SELFTEST("funnels.amount_follows_filter", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's FilteringBehaviour: the amount board ends at the filter's max stack size, an
    // "exactly" count is capped when the filter changes, "*" marks "up to" the full stack
    auto base = at(ctx.testPos(5570), 0, 1, 2);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ctx.setBlock(at(base, -1, 0, 0), "minecraft:chest");
    ctx.setBlock(base, fu::kBrassFunnel, {{fu::kFacing, cf::kEast}, {fu::kExtracting, 1}});
    co_await ctx.ticks(2);
    fd::setAmount(base, {64, false});
    common::filterslot::set(base, item(ctx, "minecraft:ender_pearl"));
    fd::clampAmountToFilter(base);
    auto a = fd::amountOf(base);
    ctx.check(a.count == 16 && !a.upTo, "an exactly-64 count is capped at the pearl's 16");
    fd::setAmount(base, {16, true});
    ctx.check(fd::amountCountLabel(base) == "*", "\"*\" for up to a full stack");
    common::filterslot::set(base, item(ctx, "minecraft:diamond_sword"));
    ctx.check(fd::amountCountLabel(base).empty(), "no count for a filter that doesn't stack");
    common::filterslot::set(base, items::empty());
    ctx.setBlock(base, "minecraft:air");
    ctx.setBlock(at(base, -1, 0, 0), "minecraft:air");
});

LV_REGISTER_SELFTEST("funnels.chute_takes_from_above", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's ChuteItemHandler: every side inserts (only belts / dropped items need an open top)
    auto base = at(ctx.testPos(5575), 0, 1, 2);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& r = region(ctx);
    ctx.setBlock(at(base, 0, 0, 0), fu::kChute);
    ctx.setBlock(at(base, 1, 1, 0), fu::kChute, {{fu::kFacing, 4}}); // diagonal, feeding the one below-west
    co_await ctx.ticks(3);
    auto rest = items::insert(r, bp(at(base, 1, 1, 0)), cf::kUp, item(ctx, "minecraft:paper", 5), false);
    ctx.check(items::isEmpty(rest), "a diagonal chute takes items from above through its item handler");
    co_await ctx.ticks(40);
    for (auto q : {at(base, 1, 1, 0), base}) {
        (void)items::extract(r, bp(q), cf::kDown, items::any(), 64, false);
        ctx.setBlock(q, "minecraft:air");
    }
    entitiesNear(r, base, "minecraft:paper", 3.f);
});
