// Selftests of the bearings unit (U21, testPos dx 2100..2199): bearings driven by a kinetic test
// source, windmills with sails, clockwork hands, turntables, sails, cart assemblers, controller
// rails, couplings and minecart contraption pickup.

#include <cmath>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/vanilla_components/StateVectorComponent.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "feature/bearings/Bearings.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/Contraption.h"
#include "feature/kinetics/Kinetics.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::bearings {

namespace {

namespace ct = ::lv::feature::contraption;
namespace k  = ::lv::feature::kinetics;
namespace it = ::lv::feature::common::items;
using lv::selftest::Context;

constexpr char const* kSource = "levitate:kinetics_test_source";

GP at(GP p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

/// A test area: `dx` in this unit's range, one block above the surface.
ll::coro::CoroTask<GP> area(Context& ctx, int dx) {
    auto p = ctx.testPos(dx);
    p.y += 2;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    co_return p;
}

BlockSource& regionOf(Context& ctx) { return ctx.player().getDimensionBlockSource(); }
std::string  blk(Context& ctx, GP const& p) { return idAt(regionOf(ctx), p); }
int          stateOf(Context& ctx, GP const& p, char const* s) { return stateAt(regionOf(ctx), p, s, -1); }

bool place(Context& ctx, GP const& p, std::string const& blockId, std::vector<std::pair<std::string, int>> const& states = {}) {
    return ctx.setBlock(p, blockId, states);
}

ItemStack stack(std::string const& itemId, int count = 1) {
    auto s = it::make(itemId, count);
    return s ? *s : ItemStack{};
}

/// Disassembles whatever a failed test left.
struct Guard {
    std::vector<ct::Id> ids;
    ~Guard() {
        for (auto id : ids)
            if (ct::find(id)) ct::disassemble(id);
    }
};

ct::Id contraptionOf(GP const& bearing) { return bearingState(bearing).value("id", ct::Id{0}); }

} // namespace

LV_REGISTER_SELFTEST("bearings.mechanical_assembles_when_powered", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 2100);
    auto bearing = at(p, 0, 1, 0);
    auto front   = at(p, 0, 2, 0);
    place(ctx, p, kSource, {{"levitate:axis", 1}});
    place(ctx, bearing, std::string(kMechanicalBearing), {{"levitate:facing", 1}});
    place(ctx, front, "minecraft:oak_planks");
    place(ctx, at(front, 1, 0, 0), "minecraft:stone"); // not attached
    track(bearing);
    Guard g;
    k::refresh(p);
    k::setGeneratedSpeed(p, 16);
    co_await ctx.ticks(6);
    auto id = contraptionOf(bearing);
    g.ids.push_back(id);
    ctx.require(id != 0 && ct::find(id), "the bearing assembled when it started turning");
    ctx.check(blk(ctx, front) == "minecraft:air", "the attached block was lifted");
    ctx.check(blk(ctx, at(front, 1, 0, 0)) == "minecraft:stone", "an unattached neighbour stays");
    double a0 = ct::find(id)->transform().angle;
    co_await ctx.ticks(10);
    double a1 = ct::find(id)->transform().angle;
    // 16 RPM = 4.8 degrees per tick
    ctx.check(std::abs((a1 - a0) - 48) < 6, "turns at 4.8 degrees per tick: " + std::to_string(a1 - a0));
    ctx.check(ct::find(id)->transform().axis == cc::Axis::Y, "about the bearing's axis");
    k::setGeneratedSpeed(p, 0);
    co_await ctx.ticks(4);
    ctx.check(!ct::find(id), "stopping places the structure (Always Place when Stopped)");
    ctx.check(blk(ctx, front) == "minecraft:oak_planks", "the planks are back in front");
    ctx.check(!bearingState(bearing).value("running", true), "not running");
});

LV_REGISTER_SELFTEST("bearings.mechanical_rotation_modes", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p       = co_await area(ctx, 2106);
    auto bearing = at(p, 0, 1, 0);
    auto front   = at(p, 0, 2, 0);
    place(ctx, p, kSource, {{"levitate:axis", 1}});
    place(ctx, bearing, std::string(kMechanicalBearing), {{"levitate:facing", 1}});
    place(ctx, front, "minecraft:oak_planks");
    track(bearing);
    Guard g;
    // Only Place when Anchor Destroyed
    ctx.require(common::valuebox::apply(ctx.player(), bearing, "mode", {0, 2}), "mode set through the value box");
    ctx.check(bearingState(bearing).value("mode", -1) == 2, "mode stored");
    k::refresh(p);
    k::setGeneratedSpeed(p, 16);
    co_await ctx.ticks(8);
    auto id = contraptionOf(bearing);
    g.ids.push_back(id);
    ctx.require(id && ct::find(id), "assembled");
    k::setGeneratedSpeed(p, 0);
    co_await ctx.ticks(4);
    ctx.check(ct::find(id) != nullptr, "stays assembled when stopped (never place)");
    // a click places it at once
    bearingUse(ctx.player(), bearing);
    ctx.check(!ct::find(id), "a click disassembles");
    ctx.check(blk(ctx, front) == "minecraft:oak_planks", "placed back");
    // Only Place near Initial Angle: stops at ~90 degrees -> keeps turning... stays assembled
    common::valuebox::apply(ctx.player(), bearing, "mode", {0, 1});
    k::setGeneratedSpeed(p, 32);
    co_await ctx.ticks(12); // ~9.6 deg/tick: ~100 degrees after assembly
    id = contraptionOf(bearing);
    g.ids.push_back(id);
    ctx.require(id && ct::find(id), "assembled again");
    double angle = std::fmod(std::abs(ct::find(id)->transform().angle), 360.0);
    k::setGeneratedSpeed(p, 0);
    co_await ctx.ticks(4);
    bool near = angle < 45 || angle > 315;
    ctx.check((ct::find(id) != nullptr) == !near, "placed only near the initial angle (angle " + std::to_string(angle) + ")");
    if (ct::find(id)) bearingUse(ctx.player(), bearing);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) ctx.cleanupBlock(at(front, dx, 0, dz));
});

LV_REGISTER_SELFTEST("bearings.mechanical_removed_places_structure", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p       = co_await area(ctx, 2112);
    auto bearing = at(p, 0, 1, 0);
    auto front   = at(p, 0, 2, 0);
    place(ctx, p, kSource, {{"levitate:axis", 1}});
    place(ctx, bearing, std::string(kMechanicalBearing), {{"levitate:facing", 1}});
    place(ctx, front, "minecraft:oak_planks");
    track(bearing);
    Guard g;
    k::refresh(p);
    k::setGeneratedSpeed(p, 16);
    co_await ctx.ticks(6);
    auto id = contraptionOf(bearing);
    g.ids.push_back(id);
    ctx.require(id && ct::find(id), "assembled");
    game::clearBlock(regionOf(ctx), bp(bearing));
    co_await ctx.ticks(3);
    ctx.check(!ct::find(id), "breaking the bearing places the structure");
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) ctx.cleanupBlock(at(front, dx, 0, dz));
});

LV_REGISTER_SELFTEST("bearings.mechanical_assembly_error", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p       = co_await area(ctx, 2116);
    auto bearing = at(p, 0, 1, 0);
    auto front   = at(p, 0, 2, 0);
    place(ctx, p, kSource, {{"levitate:axis", 1}});
    place(ctx, bearing, std::string(kMechanicalBearing), {{"levitate:facing", 1}});
    place(ctx, front, "minecraft:bedrock");
    track(bearing);
    k::refresh(p);
    k::setGeneratedSpeed(p, 16);
    co_await ctx.ticks(6);
    auto st = bearingState(bearing);
    ctx.check(!st.value("running", true), "an unmovable block stops the assembly");
    ctx.check(st.value("error", std::string()) == "create.gui.assembly.exception.unmovableBlock", "error: " + st.value("error", std::string()));
    ctx.check(blk(ctx, front) == "minecraft:bedrock", "world untouched");
    k::setGeneratedSpeed(p, 0);
});

LV_REGISTER_SELFTEST("bearings.windmill_generates", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p       = co_await area(ctx, 2120);
    auto bearing = p;
    place(ctx, bearing, std::string(kWindmillBearing), {{"levitate:facing", 1}});
    // 3x3 sails above, glued together
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) place(ctx, at(p, dx, 1, dz), "levitate:white_sail", {{"levitate:facing", 1}});
    ct::addGlue(p.dim, cc::GlueBox::of(ccp(at(p, -1, 1, -1)), ccp(at(p, 1, 1, 1))));
    track(bearing);
    Guard g;
    bearingUse(ctx.player(), bearing);
    co_await ctx.ticks(3);
    auto id = contraptionOf(bearing);
    g.ids.push_back(id);
    ctx.require(id && ct::find(id), "the windmill assembled with 9 sails");
    ctx.check(bearingState(bearing).value("sails", 0) == 9, "9 sails counted");
    co_await ctx.ticks(3);
    ctx.check(std::abs(k::theoreticalSpeedAt(bearing) - 1.f) < 1e-3, "9 sails: 1 RPM clockwise, got " + std::to_string(k::theoreticalSpeedAt(bearing)));
    // the direction box turns it the other way at once
    common::valuebox::apply(ctx.player(), bearing, "direction", {0, 1});
    co_await ctx.ticks(3);
    ctx.check(std::abs(k::theoreticalSpeedAt(bearing) + 1.f) < 1e-3, "counter-clockwise: -1 RPM, got " + std::to_string(k::theoreticalSpeedAt(bearing)));
    co_await ctx.ticks(20);
    ctx.check(std::abs(ct::find(id)->transform().angle) > 1, "the sails turn");
    bearingUse(ctx.player(), bearing);
    co_await ctx.ticks(3);
    ctx.check(!ct::find(id), "a click stops the windmill");
    ctx.check(blk(ctx, at(p, 1, 1, 0)) == "levitate:white_sail" && blk(ctx, at(p, -1, 1, -1)) == "levitate:white_sail", "sails placed back where they were");
    ctx.check(std::abs(k::theoreticalSpeedAt(bearing)) < 1e-3, "no longer generating");
    ct::clearGlue(p.dim, ccp(p), 3);
});

LV_REGISTER_SELFTEST("bearings.windmill_needs_sails", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 2126);
    place(ctx, p, std::string(kWindmillBearing), {{"levitate:facing", 1}});
    place(ctx, at(p, 0, 1, 0), "minecraft:oak_planks");
    place(ctx, at(p, 1, 1, 0), "levitate:white_sail", {{"levitate:facing", 1}});
    place(ctx, at(p, -1, 1, 0), "minecraft:white_wool");
    ct::addGlue(p.dim, cc::GlueBox::of(ccp(at(p, -1, 1, 0)), ccp(at(p, 1, 1, 0))));
    track(p);
    bearingUse(ctx.player(), p);
    co_await ctx.ticks(3);
    auto st = bearingState(p);
    ctx.check(!st.value("running", true), "2 sail-like blocks are not enough");
    ctx.check(st.value("error", std::string()) == "create.gui.assembly.exception.not_enough_sails", "error: " + st.value("error", std::string()));
    ctx.check(blk(ctx, at(p, 1, 1, 0)) == "levitate:white_sail" && blk(ctx, at(p, -1, 1, 0)) == "minecraft:white_wool", "structure stays");
    ct::clearGlue(p.dim, ccp(p), 3);
});

LV_REGISTER_SELFTEST("bearings.clockwork_hands", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p       = co_await area(ctx, 2130);
    auto bearing = at(p, 0, 1, 0);
    place(ctx, p, kSource, {{"levitate:axis", 1}});
    place(ctx, bearing, std::string(kClockworkBearing), {{"levitate:facing", 1}});
    place(ctx, at(p, 0, 2, 0), "minecraft:oak_planks"); // hour hand
    place(ctx, at(p, 0, 3, 0), "minecraft:stone");      // minute hand (not attached to the hour hand)
    track(bearing);
    Guard g;
    auto level = ll::service::getLevel();
    int  time  = level->getTime();
    level->setTime(9250); // 15:15 -> both hands at a quarter turn
    bearingUse(ctx.player(), bearing);
    co_await ctx.ticks(3);
    auto st = bearingState(bearing);
    ct::Id hour = st.value("id", ct::Id{0}), minute = st.value("second", ct::Id{0});
    g.ids = {hour, minute};
    ctx.require(hour && minute && ct::find(hour) && ct::find(minute), "two hands assembled");
    k::refresh(p);
    k::setGeneratedSpeed(p, 64);
    co_await ctx.ticks(30);
    auto norm = [](double a) { return std::fmod(std::fmod(a, 360.0) + 360.0, 360.0); };
    double h = norm(ct::find(hour)->transform().angle), m = norm(ct::find(minute)->transform().angle);
    ctx.check(std::abs(h - 270) < 1, "hour hand at 3 o'clock: " + std::to_string(h));
    ctx.check(std::abs(m - 270) < 1, "minute hand at 15 minutes: " + std::to_string(m));
    level->setTime(time);
    k::setGeneratedSpeed(p, 0);
    co_await ctx.ticks(4);
    ctx.check(!ct::find(hour) && !ct::find(minute), "stopping places both hands");
    for (int dy = 2; dy <= 3; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) ctx.cleanupBlock(at(p, dx, dy, dz));
});

LV_REGISTER_SELFTEST("bearings.turntable_turns_players", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p     = co_await area(ctx, 2136);
    auto table = at(p, 0, 1, 0);
    place(ctx, p, kSource, {{"levitate:axis", 1}});
    place(ctx, table, std::string(kTurntable));
    track(table);
    k::refresh(p);
    k::setGeneratedSpeed(p, 32);
    co_await ctx.ticks(4);
    ctx.check(std::abs(k::speedAt(table) - 32) < 1e-3, "the turntable turns at 32 RPM");
    auto& player = ctx.spawnPlayer("lv_turntable");
    player.teleport(Vec3{table.x + 0.8f, table.y + 1.0f, table.z + 0.5f}, player.getDimensionId());
    co_await ctx.ticks(10);
    auto   feet  = player.getFeetPos();
    double angle = std::atan2(-(feet.z - (table.z + .5)), feet.x - (table.x + .5)) * 180 / 3.14159265;
    ctx.check(std::abs(angle) > 20, "the player was carried around the centre (angle " + std::to_string(angle) + ")");
    k::setGeneratedSpeed(p, 0);
});

LV_REGISTER_SELFTEST("bearings.sails_dye_and_shear", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto  p = co_await area(ctx, 2140);
    auto& r = regionOf(ctx);
    for (int dx = -1; dx <= 1; ++dx) place(ctx, at(p, dx, 0, 0), "levitate:white_sail", {{"levitate:facing", 1}});
    applyDye(r, p, {0.5, 1, 0.5}, "red");
    ctx.check(blk(ctx, p) == "levitate:red_sail", "the clicked sail is dyed");
    ctx.check(stateOf(ctx, p, "levitate:facing") == 1, "facing kept");
    applyDye(r, p, {0.9, 1, 0.5}, "red");
    ctx.check(blk(ctx, at(p, 1, 0, 0)) == "levitate:red_sail" && blk(ctx, at(p, -1, 0, 0)) == "levitate:white_sail", "then the nearest neighbour");
    applyDye(r, p, {0.5, 1, 0.5}, "red");
    ctx.check(blk(ctx, at(p, -1, 0, 0)) == "levitate:red_sail", "then the whole plane");
    applyDye(r, p, {0.5, 1, 0.5}, std::nullopt);
    ctx.check(blk(ctx, p) == "levitate:sail_frame", "shears take the canvas off");
    // the sail item extends the plane next to the clicked sail
    auto& player = ctx.player();
    ctx.give(player, stack("levitate:white_sail", 4), 0);
    player.setSelectedSlot(0);
    ctx.check(sailUse(player, at(p, 1, 0, 0), 1, {0.95, 1, 0.5}, player.getSelectedItem()), "placement helper used");
    ctx.check(blk(ctx, at(p, 2, 0, 0)) == "levitate:white_sail", "a sail placed east of the clicked one");
    ctx.check(stateOf(ctx, at(p, 2, 0, 0), "levitate:facing") == 1, "with the same facing");
    ctx.cleanupBlock(at(p, 2, 0, 0));
});

LV_REGISTER_SELFTEST("bearings.cart_assembler_placement", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = co_await area(ctx, 2146);
    place(ctx, at(p, 0, -1, 0), "minecraft:stone");
    place(ctx, p, "minecraft:golden_rail", {{"rail_direction", 1}});
    auto& player = ctx.player();
    ctx.check(placeAssembler(player, p, false), "used on a rail");
    markCreativeBreak(p);
    ctx.check(blk(ctx, p) == std::string(kCartAssembler), "the rail became a cart assembler");
    ctx.check(stateOf(ctx, p, "levitate:rail_type") == 1, "it knows it was a powered rail");
    ctx.check(stateOf(ctx, p, "levitate:rail_axis") == 1, "east-west");
    place(ctx, at(p, 2, 0, 0), "minecraft:stone");
    ctx.check(placeAssembler(player, at(p, 2, 0, 0), false) && blk(ctx, at(p, 2, 0, 0)) == "minecraft:stone", "refused on a non-rail");
});

LV_REGISTER_SELFTEST("bearings.cart_contraption_follows_cart", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto  p = co_await area(ctx, 2150);
    auto& r = regionOf(ctx);
    for (int dx = -1; dx <= 6; ++dx) place(ctx, at(p, dx, -1, 0), "minecraft:stone");
    for (int dx = 1; dx <= 6; ++dx) place(ctx, at(p, dx, 0, 0), "minecraft:rail", {{"rail_direction", 1}});
    place(ctx, p, std::string(kCartAssembler), {{"levitate:rail_axis", 1}, {"levitate:rail_type", 0}, {"levitate:powered", 1}});
    place(ctx, at(p, 0, 0, 1), "minecraft:redstone_block");
    place(ctx, at(p, 0, 1, 0), "minecraft:oak_planks");
    track(p);
    markCreativeBreak(p); // no rail drop when the test area is cleared
    Guard g;
    auto* cart = spawnCart(r, {p.x + .5, p.y + cartRailY(), p.z + .5});
    ctx.require(cart != nullptr, "minecart spawned");
    auto cartId = uidOf(*cart);
    co_await ctx.ticks(12);
    auto cid = contraptionOfCart(cartId);
    ctx.require(cid.has_value() && ct::find(*cid), "the powered assembler put the planks on the cart");
    g.ids.push_back(*cid);
    ctx.check(blk(ctx, at(p, 0, 1, 0)) == "minecraft:air", "planks lifted");
    // push the cart east along the rails
    cart = actorById(cartId);
    ctx.require(cart != nullptr, "cart alive");
    cart->mBuiltInComponents->mStateVectorComponent->mPosDelta = Vec3{0.3f, 0, 0};
    co_await ctx.ticks(10);
    cart = actorById(cartId);
    ctx.require(cart && ct::find(*cid), "cart and contraption alive");
    double cx = cart->getPosition().x;
    auto   t  = ct::find(*cid)->transform();
    double wx = t.origin.x + t.offset.x + 0.5;
    ctx.check(cx > p.x + 1.5, "the cart moved east: " + std::to_string(cx - p.x));
    ctx.check(std::abs(wx - cx) < 0.6, "the structure follows the cart (" + std::to_string(wx) + " vs " + std::to_string(cx) + ")");
    // breaking the cart places the structure where it is
    cart->remove();
    co_await ctx.ticks(3);
    ctx.check(!ct::find(*cid), "the contraption is placed when its cart is gone");
    for (int dx = 0; dx <= 7; ++dx)
        for (int dz = -1; dz <= 1; ++dz) ctx.cleanupBlock(at(p, dx, 1, dz));
});

LV_REGISTER_SELFTEST("bearings.controller_rail_drives_carts", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto  p = co_await area(ctx, 2160);
    auto& r = regionOf(ctx);
    for (int dx = -1; dx <= 6; ++dx) place(ctx, at(p, dx, -1, 0), "minecraft:stone");
    for (int dx = 1; dx <= 6; ++dx) place(ctx, at(p, dx, 0, 0), "minecraft:rail", {{"rail_direction", 1}});
    // pointing east (backwards on an east-west rail), powered by a redstone block
    place(ctx, p, std::string(kControllerRail), {{"levitate:rail_axis", 1}, {"levitate:backwards", 1}});
    place(ctx, at(p, 0, 0, 1), "minecraft:redstone_block");
    track(p);
    co_await ctx.ticks(5);
    ctx.check(stateOf(ctx, p, "levitate:power") == 15, "signal strength 15");
    auto* cart = spawnCart(r, {p.x + .5, p.y + cartRailY(), p.z + .5});
    ctx.require(cart != nullptr, "minecart spawned");
    auto cartId = uidOf(*cart);
    co_await ctx.ticks(10);
    cart = actorById(cartId);
    ctx.require(cart != nullptr, "cart alive");
    ctx.check(cart->getPosition().x > p.x + 2, "the cart was sent east: " + std::to_string(cart->getPosition().x - p.x));
    cart->remove();
});

LV_REGISTER_SELFTEST("bearings.coupling_rules", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto  p = co_await area(ctx, 2166);
    auto& r = regionOf(ctx);
    for (int dx = 0; dx <= 4; ++dx) {
        place(ctx, at(p, dx, -1, 0), "minecraft:stone");
        place(ctx, at(p, dx, 0, 0), "minecraft:rail", {{"rail_direction", 1}});
    }
    auto* a = spawnCart(r, {p.x + .5, p.y + cartRailY(), p.z + .5});
    auto* b = spawnCart(r, {p.x + 3.5, p.y + cartRailY(), p.z + .5});
    ctx.require(a && b, "two minecarts");
    auto ua = uidOf(*a), ub = uidOf(*b);
    co_await ctx.ticks(2);
    ctx.check(coupleCarts(&ctx.player(), ua, ub).empty(), "coupled");
    auto len = couplingLength(ua, ub);
    ctx.check(len && std::abs(*len - 3) < 1e-6, "coupling length 3");
    ctx.check(coupleCarts(&ctx.player(), ub, ua) == "create.minecart_coupling.no_loops", "no second coupling between the same carts");
    // pushing one cart drags the other
    if (auto* ca = actorById(ua)) ca->mBuiltInComponents->mStateVectorComponent->mPosDelta = Vec3{-0.3f, 0, 0};
    co_await ctx.ticks(10);
    auto* ca = actorById(ua);
    auto* cb2 = actorById(ub);
    if (ca && cb2) {
        double d = std::abs(cb2->getPosition().x - ca->getPosition().x);
        ctx.check(std::abs(d - 3) < 0.75, "the coupling keeps its length: " + std::to_string(d));
    }
    if (ca) ca->remove();
    if (cb2) cb2->remove();
});

LV_REGISTER_SELFTEST("bearings.minecart_contraption_pickup", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto  p = co_await area(ctx, 2174);
    auto& r = regionOf(ctx);
    for (int dx = -1; dx <= 4; ++dx) place(ctx, at(p, dx, -1, 0), "minecraft:stone");
    place(ctx, p, std::string(kCartAssembler), {{"levitate:rail_axis", 1}, {"levitate:rail_type", 0}, {"levitate:powered", 1}});
    place(ctx, at(p, 0, 0, 1), "minecraft:redstone_block");
    place(ctx, at(p, 0, 1, 0), "minecraft:oak_planks");
    place(ctx, at(p, 3, 0, 0), "minecraft:rail", {{"rail_direction", 1}});
    track(p);
    markCreativeBreak(p); // no rail drop when the test area is cleared
    Guard g;
    auto* cart = spawnCart(r, {p.x + .5, p.y + cartRailY(), p.z + .5});
    ctx.require(cart != nullptr, "minecart spawned");
    auto cartId = uidOf(*cart);
    co_await ctx.ticks(12);
    auto cid = contraptionOfCart(cartId);
    ctx.require(cid && ct::find(*cid), "assembled on the cart");
    g.ids.push_back(*cid);
    auto& player = ctx.player();
    for (int s = 0; s < 9; ++s) player.setSelectedSlot(s), player.setSelectedItem(ItemStack{});
    player.setSelectedSlot(0);
    cart = actorById(cartId);
    ctx.require(cart && pickupContraption(player, *cart), "picked up with the wrench");
    co_await ctx.ticks(2);
    ctx.check(!ct::find(*cid), "the contraption is gone");
    ctx.check(blk(ctx, at(p, 0, 1, 0)) == "minecraft:air", "no block left in the world");
    ItemStack item;
    for (int s = 0; s < 36; ++s) {
        auto const& st = player.getInventory().getItem(s);
        if (it::idOf(st) == kMinecartContraption) item = st;
    }
    ctx.require(it::idOf(item) == kMinecartContraption, "the player got a minecart contraption");
    ctx.check(it::getData(item, "lv:minecart_contraption").contains("record"), "carrying the structure");
    // placing it on a rail brings it back on a new cart
    ctx.give(player, item, 0);
    player.setSelectedSlot(0);
    auto before = ct::all().size();
    ctx.check(placeMinecartContraption(player, at(p, 3, 0, 0)), "placed on a rail");
    co_await ctx.ticks(3);
    ctx.check(ct::all().size() == before + 1, "a new contraption rides a new cart");
    for (auto cidNew : ct::all()) {
        auto* c = ct::find(cidNew);
        if (c && c->anchor() == at(p, 3, 0, 0)) {
            g.ids.push_back(cidNew);
            ctx.check(c->blocks().size() == 1 && c->blocks()[0].id == "minecraft:oak_planks", "the same planks");
        }
    }
    for (int dx = -1; dx <= 5; ++dx)
        for (int dz = -1; dz <= 1; ++dz) ctx.cleanupBlock(at(p, dx, 1, dz));
});

} // namespace lv::feature::bearings
