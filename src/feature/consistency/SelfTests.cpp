// Selftests of the consistency unit (U35, SPEC §3.6.35): block states that read back as their
// values, the single fluid registry and the shared strings. testPos dx 3500..3599.

#include <string>
#include <tuple>
#include <variant>

#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "core/contraption/StateRotation.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Filter.h"
#include "feature/common/FluidRegistry.h"
#include "feature/common/Items.h"
#include "feature/contraption/Registries.h"
#include "game/Blocks.h"
#include "mod/I18n.h"
#include "selftest/SelfTest.h"

namespace lv::feature::consistency {

namespace {

using core::GlobalPos;
using selftest::Context;
namespace ft    = common::fluidtypes;
namespace items = common::items;
namespace cc    = core::contraption;

constexpr char kChute[]      = "levitate:chute";
constexpr char kSmartChute[] = "levitate:smart_chute";
constexpr char kFacing[]     = "levitate:facing";
constexpr char kShape[]      = "levitate:shape";

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

int stateAt(Context& ctx, GlobalPos const& p, char const* name) {
    auto& region = ctx.player().getDimensionBlockSource();
    auto  bp     = game::blockPos(p);
    if (!region.hasChunksAt(bp, 0, false)) return -2;
    return game::intState(region.getBlock(bp), name).value_or(-1);
}

} // namespace

// Bedrock reads integer states by their position in the state list; the chute's lists run 0..n,
// so a value set is the value read (and the funnels unit maps it to a direction).
LV_REGISTER_SELFTEST("consistency.chute_states", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(3500), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    ctx.require(co_await ctx.ensureLoaded(at(base, 8, 6, 8)), "test area corner loaded");
    for (int f = 0; f <= 4; ++f) {
        for (int s = 0; s <= 3; ++s) {
            auto p = at(base, f * 2, 6, s * 2);
            ctx.require(ctx.setBlock(p, kChute, {{kFacing, f}, {kShape, s}}), "chute placed");
            ctx.check(stateAt(ctx, p, kFacing) == f && stateAt(ctx, p, kShape) == s,
                      "chute facing " + std::to_string(f) + " shape " + std::to_string(s) + " reads back");
        }
    }
    for (int v = 0; v <= 1; ++v) {
        auto p = at(base, v * 2, 6, 8);
        ctx.require(ctx.setBlock(p, kSmartChute, {{"levitate:powered", v}}), "smart chute placed");
        ctx.check(stateAt(ctx, p, "levitate:powered") == v, "smart chute powered reads back");
    }
    // a diagonal chute facing west (state 3) feeding the chute below and east of it stays diagonal
    // (Create's ChuteBlock.updateChuteState: no inputs -> intersection)
    auto target = at(base, 1, 0, 0), diagonal = at(base, 0, 1, 0);
    ctx.require(ctx.setBlock(target, kChute), "target chute");
    ctx.require(ctx.setBlock(diagonal, kChute, {{kFacing, 3}}), "diagonal chute");
    // the same chute without a target turns straight down
    auto lone = at(base, 0, 1, 4);
    ctx.require(ctx.setBlock(lone, kChute, {{kFacing, 3}}), "lone diagonal chute");
    co_await ctx.ticks(4);
    ctx.check(stateAt(ctx, diagonal, kFacing) == 3, "the diagonal chute still faces west: " + std::to_string(stateAt(ctx, diagonal, kFacing)));
    ctx.check(stateAt(ctx, diagonal, kShape) == 2, "and is an intersection: " + std::to_string(stateAt(ctx, diagonal, kShape)));
    ctx.check(stateAt(ctx, target, kFacing) == 0, "the target chute faces down");
    ctx.check(stateAt(ctx, lone, kFacing) == 0 && stateAt(ctx, lone, kShape) == 0, "a chute without a target turns down and normal");
});

// Contraptions turn a chute's facing state as a direction (0 down, 1..4 north / south / west / east).
LV_REGISTER_SELFTEST("consistency.chute_rotation", [](Context& ctx) {
    auto const* handler = contraption::detail::rotationHandler(kChute);
    ctx.require(handler != nullptr, "the chute has a rotation handler");
    struct Case {
        int      state;
        cc::Axis axis;
        int      quarters;
        int      expected;
    };
    // north (1) a quarter turn about Y; down (0) stays down; a diagonal turned upright keeps its facing
    auto northTurned = cc::rotate(cc::Dir::North, cc::Axis::Y, 1);
    int  expected    = static_cast<int>(northTurned) - 1;
    for (auto const& c : {Case{1, cc::Axis::Y, 1, expected}, Case{0, cc::Axis::Y, 3, 0}, Case{0, cc::Axis::X, 1, 0}, Case{3, cc::Axis::Z, 1, 3}, Case{2, cc::Axis::Y, 2, 1}}) {
        cc::States s{{kFacing, std::int64_t{c.state}}};
        cc::rotateStates(s, c.axis, c.quarters);
        (*handler)(s, c.axis, c.quarters);
        auto got = std::get<std::int64_t>(s[kFacing]);
        ctx.check(got == c.expected, "facing " + std::to_string(c.state) + " turned " + std::to_string(c.quarters) + " about axis "
                                         + std::to_string(static_cast<int>(c.axis)) + ": " + std::to_string(got) + ", want " + std::to_string(c.expected));
    }
});

// One fluid registry: Create's fluids (registered by U19), their containers and names.
LV_REGISTER_SELFTEST("consistency.fluid_registry", [](Context& ctx) {
    auto const* honey = ft::find("levitate:honey");
    ctx.require(honey != nullptr, "honey is registered");
    ctx.check(honey->bucketItem == "levitate:honey_bucket" && honey->bottleItem == "minecraft:honey_bottle", "honey's containers");
    ctx.check(ft::bucketOf("minecraft:water") == "minecraft:water_bucket", "water's bucket");
    ctx.check(ft::bucketOf("minecraft:powder_snow").empty(), "powder snow fills no bucket");
    ctx.check(ft::bucketOf("levitate:tea").empty(), "builder's tea has no bucket");
    ctx.check(ft::hasBlockState("levitate:chocolate") && !ft::hasBlockState("minecraft:milk"), "world blocks");
    ctx.check(ft::find("potion:swiftness") != nullptr && ft::find("potion:swiftness") == ft::find("levitate:potion/splash/swiftness"), "both potion forms share the potion entry");
    ctx.check(ft::effectFor("levitate:potion/regular/swiftness") == ft::effectFor("potion:swiftness"), "both potion forms share the open-pipe effect");
    ctx.check(ft::kBucket == 1000 && ft::kBottle == 250, "bucket and bottle amounts");
    for (auto const& [id, fluid, amount] : {std::tuple{"levitate:honey_bucket", "levitate:honey", 1000}, std::tuple{"minecraft:honey_bottle", "levitate:honey", 250},
                                            std::tuple{"levitate:builders_tea", "levitate:tea", 250}, std::tuple{"levitate:chocolate_bucket", "levitate:chocolate", 1000}}) {
        auto stack = items::make(id);
        ctx.require(stack.has_value(), std::string("item ") + id);
        auto f = common::fluids::fluidInItem(*stack);
        ctx.check(f.fluid == fluid && f.amount == amount, std::string(id) + " holds " + std::to_string(amount) + " mB of " + fluid + ": " + f.fluid);
    }
    ctx.check(ft::displayName("levitate:honey", "en_US") == "Honey", "honey's name");
    ctx.check(ft::displayName("levitate:honey", "zh_CN") == "蜂蜜", "honey's Chinese name");
    ctx.check(ft::displayName("minecraft:water", "en_US") == "Water", "water's name");
    ctx.check(ft::displayName("levitate:potion/regular/long_swiftness", "en_US") == "Potion (long swiftness)", "potion name: " + ft::displayName("levitate:potion/regular/long_swiftness", "en_US"));
    ctx.check(ft::displayName("othermod:liquid_gold", "en_US") == "Liquid Gold", "unknown fluids are made readable");
});

// Shared strings are Create's keys with Create's text, defined once.
LV_REGISTER_SELFTEST("consistency.strings", [](Context& ctx) {
    ctx.check(translate("create.generic.unit.ticks", "?", "en_US") == "Ticks", "create.generic.unit.ticks");
    ctx.check(translate("create.generic.unit.seconds", "?", "zh_CN") == "秒", "create.generic.unit.seconds (zh_CN)");
    ctx.check(translate("create.schedule.condition.threshold.items", "?", "zh_CN") == "个", "threshold units are shared");
    ctx.check(translate("create.action.confirm", "?", "en_US") == "Confirm", "create.action.confirm");
    ctx.check(translate("lv.redstone.unit.ticks", "gone", "en_US") == "gone", "U23's copy of the time units is gone");
    ctx.check(translate("lv.equipment.confirm", "gone", "en_US") == "gone", "U31's copy of Confirm is gone");
    namespace filter = common::filter;
    ctx.check(filter::describe({"book_copy", "copy_of_original", false}, "en_US") == "is a first-generation copy", "book copy attribute: " + filter::describe({"book_copy", "copy_of_original", false}, "en_US"));
    ctx.check(filter::describe({"shulker_fill_level", "partial", true}, "en_US") == "is shulker not partially filled", "shulker attribute: " + filter::describe({"shulker_fill_level", "partial", true}, "en_US"));
    ctx.check(filter::describe({"blastable", "", false}, "en_US") == "can be Smelted in a Blast Furnace", "blastable attribute");
});

} // namespace lv::feature::consistency
