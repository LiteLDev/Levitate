// Selftests of the U45 diving gear rows (testPos dx 4590..4599): netherite toughness and
// knockback resistance, the diving boots' underwater speed and the diving advancements.

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeInstanceConstRef.h"
#include "mc/world/attribute/SharedAttributes.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/advancements/Advancements.h"
#include "feature/common/Items.h"
#include "feature/equipment/Equipment.h"
#include "selftest/SelfTest.h"

namespace {

/// testPos puts dx 45xx outside the area the selftest run keeps ticking: move into z -62..-58.
constexpr int kU45Shift = -240;

using lv::core::GlobalPos;
using lv::selftest::Context;
using SharedTypes::Legacy::ArmorSlot;
namespace eq    = lv::feature::equipment;
namespace adv   = lv::feature::advancements;
namespace items = lv::feature::common::items;

GlobalPos at(GlobalPos p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

ItemStack item(Context& ctx, std::string const& id) {
    auto s = items::make(id, 1);
    ctx.require(s.has_value(), "item " + id + " exists");
    return *s;
}

float attr(Player& p, Attribute const& a) {
    auto ref = p.getAttribute(a);
    auto const* inst = static_cast<AttributeInstance const*>(ref.mPtr);
    return inst ? inst->mCurrentValue : -1.f;
}

void strip(Player& p) {
    for (auto s : {ArmorSlot::Head, ArmorSlot::Torso, ArmorSlot::Legs, ArmorSlot::Feet}) p.setArmor(s, items::empty());
}

LV_REGISTER_SELFTEST("equipment.netherite_toughness", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    strip(p);
    co_await ctx.ticks(2);
    int   base = static_cast<Mob&>(p).getToughnessValue();
    float kb   = attr(p, SharedAttributes::KNOCKBACK_RESISTANCE());
    p.setArmor(ArmorSlot::Head, item(ctx, "levitate:netherite_diving_helmet"));
    p.setArmor(ArmorSlot::Torso, item(ctx, "levitate:netherite_backtank"));
    p.setArmor(ArmorSlot::Feet, item(ctx, "levitate:netherite_diving_boots"));
    co_await ctx.ticks(3);
    ctx.check(eq::netheriteToughness(p) == 9, "3 netherite diving pieces: toughness 9");
    ctx.check(static_cast<Mob&>(p).getToughnessValue() == base + 9, "the mob's toughness includes them");
    ctx.check(attr(p, SharedAttributes::KNOCKBACK_RESISTANCE()) > kb + .29f, "knockback resistance +0.3");
    p.setArmor(ArmorSlot::Head, item(ctx, "levitate:copper_diving_helmet"));
    co_await ctx.ticks(3);
    ctx.check(attr(p, SharedAttributes::KNOCKBACK_RESISTANCE()) < kb + .21f, "a copper helmet: +0.2 only");
    strip(p);
    co_await ctx.ticks(3);
    ctx.check(attr(p, SharedAttributes::KNOCKBACK_RESISTANCE()) < kb + .01f, "the modifier goes with the gear");
});

LV_REGISTER_SELFTEST("equipment.diving_boots_and_suit", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto base = at(ctx.testPos(4590, kU45Shift), 0, 1, 0);
    ctx.require(co_await ctx.ensureLoaded(base), "test area loaded");
    auto& p = ctx.player();
    strip(p);
    adv::revoke(p, "diving_suit");
    for (int x = -1; x <= 1; ++x)
        for (int y = 0; y <= 3; ++y)
            for (int z = -1; z <= 1; ++z) {
                bool wall = x != 0 || z != 0 || y == 0;
                ctx.setBlock(at(base, x, y, z), wall ? "minecraft:glass" : "minecraft:water");
            }
    float speed = attr(p, SharedAttributes::UNDERWATER_MOVEMENT_SPEED());
    p.setArmor(ArmorSlot::Feet, item(ctx, "levitate:copper_diving_boots"));
    auto tank = item(ctx, "levitate:copper_backtank");
    eq::setAir(tank, 100);
    p.setArmor(ArmorSlot::Torso, tank);
    p.setArmor(ArmorSlot::Head, item(ctx, "levitate:copper_diving_helmet"));
    p.teleport(Vec3{base.x + .5f, float(base.y + 1), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(10);
    ctx.check(attr(p, SharedAttributes::UNDERWATER_MOVEMENT_SPEED()) > speed * 1.8f, "diving boots: Create's underwater walking speed");
    ctx.check(adv::has(p, "diving_suit"), "helmet + backtank under water: Ready for the Depths");
    p.teleport(Vec3{base.x + 3.5f, float(base.y + 1), base.z + .5f}, DimensionType(0));
    co_await ctx.ticks(5);
    ctx.check(attr(p, SharedAttributes::UNDERWATER_MOVEMENT_SPEED()) < speed * 1.01f, "out of the water the speed is back");
    strip(p);
    adv::revoke(p, "diving_suit");
});

} // namespace
