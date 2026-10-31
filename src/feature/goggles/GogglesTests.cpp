// Selftests of the goggles unit (U8): wrench rotation / dismantling / pick-up / minecarts, the
// goggle overlay (kinetic stats, overstress hint, providers, fluid info) and its query channel.
// Selftest positions: testPos dx 800..899 (U8).

#include <algorithm>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/Mob.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/Items.h"
#include "feature/goggles/GoggleInfo.h"
#include "feature/goggles/Wrenchable.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::goggles {

namespace {

using selftest::Context;
using Facing = ScriptModuleMinecraft::ScriptFacing;

constexpr char const* kShaft    = "levitate:kinetics_test_shaft";
constexpr char const* kSource   = "levitate:kinetics_test_source";
constexpr char const* kConsumer = "levitate:kinetics_test_consumer";
constexpr int         X = 0, Y = 1, Z = 2;

core::GlobalPos at(Context& ctx, int dx, int dy = 0, int dz = 0) {
    auto p = ctx.testPos(dx, dz);
    p.y += dy;
    return p;
}

/// The test area's region: the test player stands in it (overworld).
BlockSource* overworld(Context& ctx) { return &ctx.player().getDimensionBlockSource(); }

std::string idAt(Context& ctx, core::GlobalPos const& p) { return game::blockIdAt(*overworld(ctx), game::blockPos(p)); }

int intStateAt(Context& ctx, core::GlobalPos const& p, char const* state) {
    return game::intState(overworld(ctx)->getBlock(game::blockPos(p)), state).value_or(-1);
}

ItemStack make(char const* id, int count = 1) { return common::items::make(id, count).value_or(ItemStack{}); }

int countOf(Player& p, std::string const& id) {
    int   n   = 0;
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& s = inv.getItem(i);
        if (!s.isNull() && s.getTypeName() == id) n += s.mCount;
    }
    return n;
}

/// A survival test player holding the wrench in slot 0.
SimulatedPlayer& wrenchPlayer(Context& ctx) {
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.setPlayerGameType(GameType::Survival);
    ctx.give(sp, make(wrench::kWrenchId), 0);
    sp.setSelectedSlot(0);
    sp.setSneaking(false);
    return sp;
}

/// Clicks a face of `p` with the item in slot 0, standing two blocks in front of that face so it
/// is visible (the simulated use checks the line of sight).
bool click(SimulatedPlayer& sp, core::GlobalPos const& p, Facing face) {
    static int const dx[] = {0, 0, 0, 0, -1, 1}, dz[] = {0, 0, -1, 1, 0, 0};
    int              f    = static_cast<int>(face);
    Vec3             stand{float(p.x) + 0.5f + 2.f * float(dx[f]), float(p.y), float(p.z) + 0.5f + 2.f * float(dz[f])};
    if (face == Facing::Up) stand = {float(p.x) + 0.5f, float(p.y) + 1.f, float(p.z) + 2.5f};
    if (face == Facing::Down) stand = {float(p.x) + 0.5f, float(p.y) - 2.f, float(p.z) + 2.5f};
    sp.teleport(stand, DimensionType(p.dim));
    Vec3 hit{0.5f, 0.5f, 0.5f};
    switch (face) {
    case Facing::Up:
        hit.y = 1;
        break;
    case Facing::Down:
        hit.y = 0;
        break;
    case Facing::North:
        hit.z = 0;
        break;
    case Facing::South:
        hit.z = 1;
        break;
    case Facing::West:
        hit.x = 0;
        break;
    default:
        hit.x = 1;
        break;
    }
    sp.simulateLookAt(Vec3{float(p.x) + hit.x, float(p.y) + hit.y, float(p.z) + hit.z}, sim::LookDuration::Instant);
    return sp.simulateUseItemInSlotOnBlock(0, game::blockPos(p), face, hit);
}

bool contains(Tooltip const& t, std::string const& needle) {
    return std::any_of(t.begin(), t.end(), [&](std::string const& l) { return l.find(needle) != std::string::npos; });
}

std::string dump(Tooltip const& t) {
    std::string s;
    for (auto const& l : t) s += "[" + core::goggles::stripFormatting(l) + "]";
    return s;
}

/// Turns on the test provider of `goggles.overlay_provider` only while that test runs.
bool& providerActive() {
    static bool b = false;
    return b;
}

LV_REGISTER_GOGGLE_INFO(
    "minecraft:cauldron",
    {
        .goggle =
            [](InfoContext const& c, Tooltip& t) {
                if (!providerActive()) return false;
                t.push_back(line("Test provider"));
                return fluidTooltip(c, t);
            },
    }
);

} // namespace

LV_REGISTER_SELFTEST("goggles.registry", [](Context& ctx) {
    ctx.require(common::items::make(wrench::kWrenchId).has_value(), "levitate:wrench is in the behavior pack");
    ctx.require(common::items::make("levitate:goggles").has_value(), "levitate:goggles is in the behavior pack");
    ctx.check(wrench::isWrenchable(kShaft), "kinetic blocks are wrenchable");
    for (auto const* id : {"levitate:sticker", "levitate:linear_chassis", "levitate:radial_chassis", "levitate:track"})
        ctx.check(wrench::isWrenchable(id), std::string(id) + " is wrenchable");
    ctx.check(!wrench::isWrenchable("minecraft:stone"), "stone is not wrenchable");
    ctx.check(wrench::canWrenchPickup("minecraft:lever") && wrench::canWrenchPickup("minecraft:oak_button"), "#create:wrench_pickup");
    ctx.check(!wrench::canWrenchPickup("minecraft:stone"), "stone can't be picked up");
    auto gen = generatorSpeedOf("levitate:creative_motor");
    ctx.check(gen && gen->rpm == 256 && gen->mayGenerateLess, "creative motor generates up to 256 RPM");
});

LV_REGISTER_SELFTEST("goggles.wrench_rotates_axis", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto src = at(ctx, 800), shaft = at(ctx, 801);
    ctx.require(co_await ctx.ensureLoaded(shaft), "test area loaded");
    auto& sp = wrenchPlayer(ctx);
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", X}}), "place the source");
    ctx.require(ctx.setBlock(shaft, kShaft, {{"levitate:axis", X}}), "place the shaft");
    co_await ctx.ticks(2);
    ctx.check(kinetics::speedAt(shaft) == 16, "the shaft turns before the wrench");

    ctx.require(click(sp, shaft, Facing::Up), "wrench the top face");
    co_await ctx.ticks(2);
    ctx.check(intStateAt(ctx, shaft, "levitate:axis") == Z, "x axis clicked on top turns to z, got " + std::to_string(intStateAt(ctx, shaft, "levitate:axis")));
    ctx.check(kinetics::speedAt(shaft) == 0, "the turned shaft left the network");

    co_await ctx.ticks(10); // one action per click; the simulated player has a use delay
    click(sp, shaft, Facing::East);
    co_await ctx.ticks(2);
    ctx.check(intStateAt(ctx, shaft, "levitate:axis") == Y, "z axis clicked on the east face turns to y");

    co_await ctx.ticks(10);
    click(sp, shaft, Facing::North);
    co_await ctx.ticks(2);
    ctx.check(intStateAt(ctx, shaft, "levitate:axis") == X, "y axis clicked on the north face turns to x");
    co_await ctx.ticks(2);
    ctx.check(kinetics::speedAt(shaft) == 16, "back in line: turns again");
});

LV_REGISTER_SELFTEST("goggles.wrench_rotates_facing", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx, 810);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = wrenchPlayer(ctx);
    ctx.require(ctx.setBlock(p, "levitate:sticker", {{"levitate:facing", 2}}), "place a sticker facing north");
    ctx.require(click(sp, p, Facing::Up), "wrench the top face");
    co_await ctx.ticks(2);
    ctx.check(intStateAt(ctx, p, "levitate:facing") == 5, "north turns to east about the vertical axis");
    co_await ctx.ticks(10);
    click(sp, p, Facing::West);
    co_await ctx.ticks(2);
    ctx.check(intStateAt(ctx, p, "levitate:facing") == 5, "a facing along the clicked axis stays");
});

LV_REGISTER_SELFTEST("goggles.wrench_dismantles", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto shaft = at(ctx, 820), hopper = at(ctx, 822), stone = at(ctx, 824);
    ctx.require(co_await ctx.ensureLoaded(shaft), "test area loaded");
    auto& sp = wrenchPlayer(ctx);
    ctx.require(ctx.setBlock(shaft, kShaft, {{"levitate:axis", X}}), "place a shaft");
    ctx.require(ctx.setBlock(hopper, "minecraft:hopper"), "place a hopper");
    ctx.require(ctx.setBlock(stone, "minecraft:stone"), "place stone");
    int shafts = countOf(sp, kShaft), hoppers = countOf(sp, "minecraft:hopper");

    sp.setSneaking(true);
    ctx.require(click(sp, shaft, Facing::Up), "sneak-wrench the shaft");
    co_await ctx.ticks(2);
    ctx.check(idAt(ctx, shaft) == "minecraft:air", "the shaft is gone, got " + idAt(ctx, shaft));
    ctx.check(countOf(sp, kShaft) == shafts + 1, "the shaft went into the inventory");

    co_await ctx.ticks(10);
    click(sp, hopper, Facing::Up);
    co_await ctx.ticks(2);
    ctx.check(idAt(ctx, hopper) == "minecraft:air", "#create:wrench_pickup: the hopper is picked up");
    ctx.check(countOf(sp, "minecraft:hopper") == hoppers + 1, "the hopper went into the inventory");

    // U42: the drops come from the block's loot table (a repeater drops the repeater item)
    auto repeater = at(ctx, 826);
    ctx.require(ctx.setBlock(repeater, "minecraft:unpowered_repeater"), "place a repeater");
    int repeaters = countOf(sp, "minecraft:repeater");
    co_await ctx.ticks(10);
    click(sp, repeater, Facing::Up);
    co_await ctx.ticks(2);
    ctx.check(idAt(ctx, repeater) == "minecraft:air", "the repeater is picked up");
    ctx.check(countOf(sp, "minecraft:repeater") == repeaters + 1, "its loot (the repeater item) went into the inventory");

    co_await ctx.ticks(10);
    click(sp, stone, Facing::Up); // refused: the game handles it
    co_await ctx.ticks(2);
    ctx.check(idAt(ctx, stone) == "minecraft:stone", "stone is not picked up");
    sp.setSneaking(false);
});

LV_REGISTER_SELFTEST("goggles.overlay_kinetic", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto src = at(ctx, 830), user = at(ctx, 831);
    ctx.require(co_await ctx.ensureLoaded(user), "test area loaded");
    auto& p = ctx.player();
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", X}}), "place the source");
    ctx.require(ctx.setBlock(user, kConsumer, {{"levitate:axis", X}}), "place the consumer");
    co_await ctx.ticks(2);

    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, ItemStack{});
    ctx.check(!isWearingGoggles(p), "no goggles");
    ctx.check(overlayFor(p, user).lines.empty(), "without goggles a running consumer shows nothing");

    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, make("levitate:goggles"));
    ctx.require(isWearingGoggles(p), "goggles in the head slot");
    auto consumer = overlayFor(p, user).lines;
    ctx.check(contains(consumer, "Kinetic Stats:") && contains(consumer, "64su"), "consumer: 4 SU/RPM x 16 RPM = 64su: " + dump(consumer));
    auto source = overlayFor(p, src).lines;
    ctx.check(contains(source, "Generator Stats:") && contains(source, "128su"), "source: 8 SU/RPM x 16 RPM = 128su: " + dump(source));
    ctx.check(!source.empty() && source[0].starts_with("    "), "lines are indented like Create's forGoggles");
    ctx.info("consumer overlay: " + dump(consumer));
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, ItemStack{});
});

LV_REGISTER_SELFTEST("goggles.overlay_overstressed", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto src = at(ctx, 840);
    ctx.require(co_await ctx.ensureLoaded(src), "test area loaded");
    auto& p = ctx.player();
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", X}}), "place the source");
    for (int i = 1; i <= 3; ++i) ctx.setBlock(at(ctx, 840 + i), kConsumer, {{"levitate:axis", X}}); // 192 su > 128 su
    co_await ctx.ticks(3);
    ctx.require(kinetics::isOverstressed(at(ctx, 842)), "the network is overstressed");

    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, ItemStack{});
    auto plain = overlayFor(p, at(ctx, 842)).lines;
    ctx.check(!plain.empty() && contains(plain, "Overstressed"), "without goggles: the overstressed hint: " + dump(plain));
    ctx.check(contains(plain, "overstressed") && contains(plain, "§f"), "the hint highlights words: " + dump(plain));
    ctx.check(!contains(plain, "Kinetic Stats:"), "without goggles: no stats");

    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, make("levitate:goggles"));
    auto full = overlayFor(p, at(ctx, 842)).lines;
    ctx.check(contains(full, "Kinetic Stats:") && contains(full, "Overstressed"), "with goggles: stats and the hint: " + dump(full));
    ctx.check(std::find(full.begin(), full.end(), std::string{}) != full.end(), "a blank line separates them");
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, ItemStack{});
});

LV_REGISTER_SELFTEST("goggles.overlay_provider", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto cauldron = at(ctx, 850);
    ctx.require(co_await ctx.ensureLoaded(cauldron), "test area loaded");
    auto& p = ctx.player();
    ctx.require(ctx.setBlock(cauldron, "minecraft:cauldron"), "place a cauldron");
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, make("levitate:goggles"));
    providerActive() = true;
    auto empty       = overlayFor(p, cauldron).lines;
    ctx.check(contains(empty, "Test provider") && contains(empty, "Fluid Container Info:"), "provider lines: " + dump(empty));
    ctx.check(contains(empty, "Capacity: ") && contains(empty, "1,000mB"), "an empty single tank shows its capacity: " + dump(empty));
    providerActive() = false;
    ctx.check(overlayFor(p, cauldron).lines.empty(), "a vanilla block without providers shows nothing");
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, ItemStack{});
});

LV_REGISTER_SELFTEST("goggles.overlay_query", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto src = at(ctx, 860), user = at(ctx, 861);
    ctx.require(co_await ctx.ensureLoaded(user), "test area loaded");
    auto& p = ctx.player();
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", X}}), "place the source");
    ctx.require(ctx.setBlock(user, kConsumer, {{"levitate:axis", X}}), "place the consumer");
    co_await ctx.ticks(2);
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, make("levitate:goggles"));
    net::setTestCompanion(p, true);
    (void)net::takeTestMessages(p);
    net::injectFromClient(p, "goggles.query", {{"pos", {user.dim, user.x, user.y, user.z}}});
    auto msgs = net::takeTestMessages(p);
    net::setTestCompanion(p, false);
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, ItemStack{});
    auto it = std::find_if(msgs.begin(), msgs.end(), [](auto const& m) { return m.channel == "goggles.info"; });
    ctx.require(it != msgs.end(), "the server answers goggles.info");
    auto lines = it->payload.value("lines", nlohmann::json::array());
    ctx.check(lines.size() >= 3, "the answer carries the overlay lines: " + lines.dump());
    ctx.check(it->payload.value("icon", std::string{}) == "levitate:goggles", "the default icon is the goggles");
});

LV_REGISTER_SELFTEST("goggles.wrench_breaks_minecarts", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = at(ctx, 870, 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp     = wrenchPlayer(ctx);
    auto* region = overworld(ctx);
    auto  level  = ll::service::getLevel();
    ctx.require(region && level, "overworld");
    // a minecart is no mob: the projectile spawner creates any actor
    Actor* cart = level->getSpawner().spawnProjectile(
        *region,
        ActorDefinitionIdentifier("minecraft:minecart"),
        nullptr,
        Vec3{float(p.x) + 0.5f, float(p.y), float(p.z) + 0.5f},
        Vec3{0, 0, 0}
    );
    if (!cart) {
        ctx.info("minecarts can't be spawned here; skipped");
        co_return;
    }
    auto id = cart->getOrCreateUniqueID();
    co_await ctx.ticks(2);
    auto* target = level->fetchEntity(id, false);
    ctx.require(target != nullptr, "the minecart exists");
    sp.teleport(Vec3{float(p.x) - 1.5f, float(p.y), float(p.z) + 0.5f}, DimensionType(p.dim));
    sp.simulateLookAt(*target, sim::LookDuration::Instant);
    co_await ctx.ticks(10);
    bool attacked = sp.simulateAttack(target);
    ctx.info("attacking a " + target->getTypeName() + ": " + (attacked ? "hit" : "no hit"));
    co_await ctx.ticks(2);
    auto* after = level->fetchEntity(id, false);
    ctx.check(!after || !after->isAlive(), "one hit with the wrench breaks the minecart");
    if (after && after->isAlive()) after->remove();
});

// ---- U52: alignment with Create 6.0.10 (testPos dx 5286..5299) ----------------------------------------------------

// K11, K12, K13: the wrench pickup tag, speed requirements and generator speeds of Create's blocks
LV_REGISTER_SELFTEST("goggles.create_tables", [](Context& ctx) {
    for (auto const* id : {"minecraft:redstone_lamp", "minecraft:lit_redstone_lamp", "levitate:andesite_bars", "levitate:brass_bars",
                           "levitate:copper_bars", "levitate:industrial_iron_block", "levitate:weathered_iron_block"})
        ctx.check(wrench::canWrenchPickup(id), std::string(id) + " is in #create:wrench_pickup");
    ctx.check(speedRequirementOf("levitate:mechanical_mixer") == SpeedLevel::Medium, "the mixer needs medium speed");
    ctx.check(speedRequirementOf("levitate:display_board") == SpeedLevel::Medium, "the display board needs medium speed");
    ctx.check(speedRequirementOf("levitate:mechanical_press") == SpeedLevel::None, "the press has no requirement");
    auto windmill = generatorSpeedOf("levitate:windmill_bearing");
    ctx.check(windmill && windmill->rpm == 16 && windmill->mayGenerateLess, "windmill bearing: up to 16 RPM");
    auto engine = generatorSpeedOf("levitate:steam_engine");
    ctx.check(engine && engine->rpm == 64 && engine->mayGenerateLess, "steam engine: up to 64 RPM");
    ctx.check(kinetics::stressCapacityOf("levitate:steam_engine") == 1024, "steam engine capacity 1024 (the item's stats)");
});

// K12: a mixer turning slower than medium speed shows Create's speed requirement hint
LV_REGISTER_SELFTEST("goggles.speed_requirement_hint", [](Context& ctx) -> ll::coro::CoroTask<> {
    // the mixer has no shaft: a cog beside it meshes with its integrated cog, a source below drives the cog
    auto mixer = at(ctx, 5288, 1), cog = at(ctx, 5289, 1), src = at(ctx, 5289, 0);
    ctx.require(co_await ctx.ensureLoaded(mixer), "test area loaded");
    if (!game::blockWith("levitate:mechanical_mixer")) {
        ctx.info("no mechanical mixer: not tested");
        co_return;
    }
    auto& p = ctx.player();
    ctx.require(ctx.setBlock(mixer, "levitate:mechanical_mixer"), "a mixer");
    ctx.require(ctx.setBlock(cog, "levitate:kinetics_test_cog", {{"levitate:axis", Y}}), "a cog beside it");
    ctx.require(ctx.setBlock(src, kSource, {{"levitate:axis", Y}}), "a 16 RPM source under the cog");
    co_await ctx.ticks(3);
    ctx.require(kinetics::speedAt(mixer) != 0, "the mixer turns, got " + std::to_string(kinetics::speedAt(mixer)));
    p.setArmor(SharedTypes::Legacy::ArmorSlot::Head, ItemStack{});
    auto lines = overlayFor(p, mixer).lines;
    ctx.check(contains(lines, "Speed Requirement:") && contains(lines, "enough"), "16 RPM < 30: the hint: " + dump(lines));
});

} // namespace lv::feature::goggles
