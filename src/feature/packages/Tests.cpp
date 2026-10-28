// Selftests of the packages unit (U25, testPos dx 2500..2599).

#include <algorithm>

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/SignBlockActor.h"
#include "mc/world/level/block/actor/SignTextSide.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "core/packages/Address.h"
#include "feature/common/BlockData.h"
#include "feature/common/Capabilities.h"
#include "feature/common/Items.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/packages/Packages.h"
#include "feature/packages/PackagesInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::selftest::Context;
namespace pk    = lv::feature::packages;
namespace items = lv::feature::common::items;
using lv::core::GlobalPos;

GlobalPos at(Context& ctx, int dx, int dy = 0, int dz = 0) {
    auto p = ctx.testPos(dx, dz);
    p.y += dy;
    return p;
}

ItemStack stack(char const* id, int count) { return items::make(id, count).value_or(ItemStack::EMPTY_ITEM()); }

int countIn(GlobalPos const& p, char const* id) {
    auto* region = pk::regionOf(p.dim);
    if (!region) return 0;
    auto h = lv::feature::common::items::find({*region, lv::game::blockPos(p), -1});
    if (!h) return 0;
    lv::feature::common::Access a{*region, lv::game::blockPos(p), -1};
    int n = 0;
    for (int i = 0; i < h->slots(a); ++i) {
        auto s = h->getSlot(a, i);
        if (items::idOf(s) == id) n += items::countOf(s);
    }
    return n;
}

bool putIntoChest(GlobalPos const& p, ItemStack const& s) {
    auto* region = pk::regionOf(p.dim);
    return region && items::isEmpty(lv::feature::common::items::insert(*region, lv::game::blockPos(p), -1, s, false));
}

bool writeSign(GlobalPos const& p, std::string const& text) {
    auto* region = pk::regionOf(p.dim);
    if (!region) return false;
    auto* actor = region->getBlockEntity(lv::game::blockPos(p));
    if (!actor || actor->getType() != BlockActorType::Sign) return false;
    static_cast<SignBlockActor*>(actor)->setMessageForServerScripingOnly(SignTextSide::Front, text, "");
    return true;
}

/// A packager at `p` facing `facing` (Bedrock face id), placed like a player would.
bool placePackager(Context& ctx, GlobalPos const& p, int facing, char const* id = pk::kPackagerId) {
    if (!ctx.setBlock(p, id, {{pk::kFacing, facing}})) return false;
    pk::track(p, pk::kindOf(id));
    pk::onPackagerPlaced(p, facing);
    return true;
}

std::string inventoryText(Player& player) {
    std::string out;
    auto&       inv = player.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i)
        if (!items::isEmpty(inv.getItem(i))) out += std::to_string(i) + "=" + items::idOf(inv.getItem(i)) + "x" + std::to_string(items::countOf(inv.getItem(i))) + " ";
    return out;
}

int countPlayer(Player& player, std::string const& id) {
    int  n   = 0;
    auto& inv = player.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i)
        if (items::idOf(inv.getItem(i)) == id) n += items::countOf(inv.getItem(i));
    return n;
}

} // namespace

LV_REGISTER_SELFTEST("packages.registry", [](Context& ctx) {
    for (char const* id : {pk::kPackagerId, pk::kRepackagerId, pk::kFrogportId, pk::kChainConveyorId})
        ctx.check(lv::game::blockWith(id) != nullptr, std::string(id) + " is registered");
    for (auto const& id : pk::postboxIds()) ctx.check(lv::game::blockWith(id) != nullptr, id + " is registered");
    for (auto const& id : pk::packageIds()) ctx.check(items::make(id).has_value(), id + " is registered");
    ctx.check(items::make(pk::kPackageFilterId).has_value(), "package filter is registered");
    ctx.check(lv::feature::kinetics::specOf(pk::kChainConveyorId) != nullptr, "chain conveyor is kinetic");
    ctx.check(pk::postboxIds().size() == 16 && pk::packageIds().size() == 14, "16 postboxes, 14 box styles");
});

LV_REGISTER_SELFTEST("packages.package_items", [](Context& ctx) {
    auto box = pk::containing({stack("minecraft:diamond", 70), stack("minecraft:stick", 3)});
    ctx.require(pk::isPackage(box), "containing() makes a package");
    auto contents = pk::contentsOf(box);
    ctx.check(contents.size() == 9, "9 slots");
    ctx.check(items::countOf(contents[0]) == 64 && items::countOf(contents[1]) == 6, "70 diamonds take two slots");
    ctx.check(items::idOf(contents[2]) == "minecraft:stick", "sticks in the third slot");
    ctx.check(pk::addressOf(box).empty(), "no address yet");
    pk::setAddress(box, "Storage");
    ctx.check(pk::addressOf(box) == "Storage", "address stored");
    ctx.check(pk::matchAddress(pk::addressOf(box), "Stor*"), "glob match");
    pk::OrderData d;
    d.orderId       = 42;
    d.fragmentIndex = 3;
    d.isFinal       = true;
    pk::setOrder(box, d);
    auto o = pk::orderOf(box);
    ctx.check(o && o->orderId == 42 && o->fragmentIndex == 3 && o->isFinal, "order data round trip");
    pk::clearAddress(box);
    ctx.check(pk::addressOf(box).empty(), "address cleared");
    ctx.check(!pk::fitsInPackage(box), "packages don't fit into packages");
    ctx.check(pk::fitsInPackage(stack("minecraft:stone", 1)), "stone fits");
    // package filter (Create's PackageFilterItemStack)
    auto filter = stack(pk::kPackageFilterId, 1);
    auto addressed = pk::containing({stack("minecraft:stone", 1)});
    pk::setAddress(addressed, "farm_1");
    ctx.check(!pk::filterMatches(filter, addressed), "an empty package filter only matches itself");
    ctx.check(pk::filterMatches(filter, filter), "an empty package filter matches package filters");
    pk::setAddress(filter, "farm_*");
    ctx.check(pk::filterMatches(filter, addressed), "address filter matches the package");
    ctx.check(!pk::filterMatches(filter, stack("minecraft:stone", 1)), "address filter rejects non-packages");
});

LV_REGISTER_SELFTEST("packages.packager_redstone", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto chest    = at(ctx, 2500);
    auto packager = at(ctx, 2501);
    auto sign     = at(ctx, 2501, 0, 1);
    auto power    = at(ctx, 2501, 1);
    ctx.require(co_await ctx.ensureLoaded(packager), "test area loaded");
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "chest placed");
    ctx.require(placePackager(ctx, packager, 5), "packager placed (facing east, chest behind)");
    ctx.require(putIntoChest(chest, stack("minecraft:cobblestone", 40)) && putIntoChest(chest, stack("minecraft:iron_ingot", 5)), "chest filled");
    ctx.require(ctx.setBlock(sign, "minecraft:standing_sign"), "sign placed");
    co_await ctx.ticks(1);
    ctx.require(writeSign(sign, "Factory\nNorth"), "sign written");
    ctx.require(ctx.setBlock(power, "minecraft:redstone_block"), "redstone block placed");
    co_await ctx.ticks(25);
    auto& s = pk::packager(packager);
    ctx.require(pk::isPackage(s.held), "the packager holds a package");
    ctx.check(pk::addressOf(s.held) == "Factory North", "address from the sign: '" + pk::addressOf(s.held) + "'");
    int cobble = 0, iron = 0;
    for (auto const& c : pk::contentsOf(s.held)) {
        if (items::idOf(c) == "minecraft:cobblestone") cobble += items::countOf(c);
        if (items::idOf(c) == "minecraft:iron_ingot") iron += items::countOf(c);
    }
    ctx.check(cobble == 40 && iron == 5, "everything was packed");
    ctx.check(countIn(chest, "minecraft:cobblestone") == 0, "the chest is empty");
    ctx.check(pk::stateAt(packager, pk::kPowered).value_or(0) == 1, "powered state");
    // a held package blocks the next pulse; the frogport / player takes it out through the capability
    auto* region = pk::regionOf(packager.dim);
    auto  taken  = lv::feature::common::items::extract(*region, lv::game::blockPos(packager), 1, items::any(), 1, false);
    ctx.check(pk::isPackage(taken), "the package can be extracted");
    ctx.check(items::isEmpty(pk::packager(packager).held), "the packager is empty again");
    // and unpacked again: back into the chest
    ctx.setBlock(power, "minecraft:air");
    co_await ctx.ticks(2);
    auto rest = lv::feature::common::items::insert(*region, lv::game::blockPos(packager), 1, taken, false);
    ctx.check(items::isEmpty(rest), "the packager accepts the package");
    co_await ctx.ticks(2);
    ctx.check(countIn(chest, "minecraft:cobblestone") == 40 && countIn(chest, "minecraft:iron_ingot") == 5, "the contents are back in the chest");
    ctx.check(pk::packager(packager).animationInward, "unpacking animation");
    co_await ctx.ticks(25);
});

LV_REGISTER_SELFTEST("packages.packager_player", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto chest    = at(ctx, 2505);
    auto packager = at(ctx, 2506);
    ctx.require(co_await ctx.ensureLoaded(packager), "test area loaded");
    ctx.require(ctx.setBlock(chest, "minecraft:barrel"), "barrel placed");
    ctx.require(placePackager(ctx, packager, 5), "packager placed");
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.teleport(Vec3{packager.x + 0.5f, packager.y + 0.f, packager.z + 2.5f}, DimensionType(packager.dim));
    co_await ctx.ticks(2);
    auto box = pk::containing({stack("minecraft:apple", 7)});
    ctx.give(sp, box, 0);
    sp.simulateSetItem(box, true, 0);
    co_await ctx.ticks(1);
    sp.simulateUseItemInSlotOnBlock(0, lv::game::blockPos(packager), ScriptModuleMinecraft::ScriptFacing::Up, Vec3{0.5f, 1.f, 0.5f});
    co_await ctx.ticks(3);
    ctx.check(countIn(chest, "minecraft:apple") == 7, "right-clicking with a package unpacks it into the barrel");
    ctx.check(countPlayer(sp, items::idOf(box)) == 0, "the package was used up");
    co_await ctx.ticks(22);
    // a held package is taken with a right-click
    auto& s = pk::packager(packager);
    s.held  = pk::containing({stack("minecraft:bread", 2)});
    sp.simulateSetItem(ItemStack::EMPTY_ITEM(), true, 0);
    co_await ctx.ticks(6);
    pk::usePackager(sp, packager); // an empty-hand click (SimulatedPlayers can't use an empty hand on a block)
    co_await ctx.ticks(3);
    ctx.check(items::isEmpty(pk::packager(packager).held), "the player took the package");
    int boxes = 0;
    for (auto const& id : pk::packageIds()) boxes += countPlayer(sp, id);
    ctx.check(boxes == 1, "the package is in the player's inventory: " + inventoryText(sp));
});

LV_REGISTER_SELFTEST("packages.open_package", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = at(ctx, 2510);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    auto& sp  = static_cast<SimulatedPlayer&>(ctx.player());
    auto  box = pk::containing({stack("minecraft:emerald", 5), stack("minecraft:coal", 9)});
    sp.simulateSetItem(box, true, 0);
    sp.simulateSneaking();
    co_await ctx.ticks(2);
    sp.simulateUseItemInSlot(0);
    co_await ctx.ticks(2);
    sp.simulateStopSneaking();
    ctx.info("inventory: " + inventoryText(sp));
    ctx.check(countPlayer(sp, "minecraft:emerald") == 5 && countPlayer(sp, "minecraft:coal") == 9, "sneak-use opens the package into the inventory");
    ctx.check(countPlayer(sp, items::idOf(box)) == 0, "the package is gone");
});

LV_REGISTER_SELFTEST("packages.repackager", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto chest = at(ctx, 2515);
    auto rp    = at(ctx, 2516);
    auto power = at(ctx, 2516, 1);
    ctx.require(co_await ctx.ensureLoaded(rp), "test area loaded");
    ctx.require(ctx.setBlock(chest, "minecraft:chest"), "chest placed");
    ctx.require(placePackager(ctx, rp, 5, pk::kRepackagerId), "re-packager placed");
    // an order of two fragments (link 0, fragments 0 and 1)
    auto a = pk::containing({stack("minecraft:gold_ingot", 10)});
    auto b = pk::containing({stack("minecraft:gold_ingot", 6), stack("minecraft:redstone", 4)});
    for (auto* box : {&a, &b}) pk::setAddress(*box, "Smeltery");
    pk::OrderData d;
    d.orderId     = 77;
    d.isFinalLink = true;
    d.fragmentIndex = 0;
    pk::setOrder(a, d);
    d.fragmentIndex = 1;
    d.isFinal       = true;
    d.context = {{"stacks", nlohmann::json::array({{{"item", items::toJson(stack("minecraft:redstone", 1))}, {"count", 4}},
                                                   {{"item", items::toJson(stack("minecraft:gold_ingot", 1))}, {"count", 16}}})},
                 {"crafts", nlohmann::json::array()}};
    pk::setOrder(b, d);
    ctx.require(putIntoChest(chest, a) && putIntoChest(chest, b), "fragments in the chest");
    ctx.require(ctx.setBlock(power, "minecraft:redstone_block"), "powered");
    co_await ctx.ticks(30);
    auto& s = pk::packager(rp);
    ItemStack out = s.held;
    if (!pk::isPackage(out) && !s.queue.empty()) out = s.queue.front().box;
    ctx.require(pk::isPackage(out), "the re-packager made the final package");
    auto contents = pk::contentsOf(out);
    ctx.check(items::idOf(contents[0]) == "minecraft:redstone" && items::countOf(contents[0]) == 4, "ordered stacks first (redstone)");
    ctx.check(items::idOf(contents[1]) == "minecraft:gold_ingot" && items::countOf(contents[1]) == 16, "gold merged");
    ctx.check(pk::addressOf(out) == "Smeltery", "address kept");
    auto o = pk::orderOf(out);
    ctx.check(o && o->orderId == 77 && o->isFinal && o->isFinalLink, "final order data");
    ctx.check(countIn(chest, pk::packageIds()[0].c_str()) + countIn(chest, pk::packageIds()[1].c_str()) + countIn(chest, pk::packageIds()[2].c_str())
                      + countIn(chest, pk::packageIds()[3].c_str())
                  == 0,
              "the fragments left the chest");
    ctx.setBlock(power, "minecraft:air");
    co_await ctx.ticks(25);
});

LV_REGISTER_SELFTEST("packages.chain_connect", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto a    = at(ctx, 2520);
    auto b    = at(ctx, 2526);
    auto near = at(ctx, 2522);
    ctx.require(co_await ctx.ensureLoaded(b), "test area loaded");
    ctx.require(ctx.setBlock(a, pk::kChainConveyorId) && ctx.setBlock(b, pk::kChainConveyorId), "conveyors placed");
    ctx.require(ctx.setBlock(near, pk::kChainConveyorId), "a third conveyor 2 blocks away");
    pk::track(a, pk::Kind::ChainConveyor);
    pk::track(b, pk::Kind::ChainConveyor);
    pk::track(near, pk::Kind::ChainConveyor);
    co_await ctx.ticks(1);
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    sp.teleport(Vec3{a.x + 3.5f, a.y + 0.f, a.z + 2.5f}, DimensionType(a.dim));
    ctx.require(!pk::chainItemIds().empty(), "the chain item exists");
    auto chains = stack(pk::chainItemIds().front().c_str(), 10);
    sp.simulateSetItem(chains, true, 0);
    co_await ctx.ticks(2);
    ctx.info("held: " + items::idOf(sp.getSelectedItem()) + " inventory: " + inventoryText(sp));
    // too close: fails, keeps the chains (the GameMode hook calls useConveyorWithChain; SimulatedPlayers
    // can't click every block with a block item, so the clicks are made directly)
    pk::useConveyorWithChain(sp, a);
    co_await ctx.ticks(6);
    pk::useConveyorWithChain(sp, near);
    co_await ctx.ticks(6);
    ctx.check(pk::conveyor(a).connections.empty(), "too close: not connected");
    // a valid pair: chains are used (6 blocks -> 2 chains)
    pk::useConveyorWithChain(sp, a);
    co_await ctx.ticks(6);
    pk::useConveyorWithChain(sp, b);
    co_await ctx.ticks(6);
    ctx.check(pk::conveyor(a).connections.contains({6, 0, 0}) && pk::conveyor(b).connections.contains({-6, 0, 0}), "connected both ways");
    int left = countPlayer(sp, pk::chainItemIds().front());
    ctx.check(sp.isCreative() || left == 8, "two chains used, " + std::to_string(left) + " left");
    // the kinetic network spans the chain
    auto members = lv::feature::kinetics::networkMembers(a);
    ctx.check(std::any_of(members.begin(), members.end(), [&](GlobalPos const& m) { return m == b; }), "one kinetic network");
    // breaking a conveyor removes the chain on the other end
    ctx.setBlock(a, "minecraft:air");
    co_await ctx.ticks(4);
    ctx.check(pk::conveyor(b).connections.empty(), "the other end lost its connection");
});

LV_REGISTER_SELFTEST("packages.chain_routing", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto a      = at(ctx, 2530);
    auto b      = at(ctx, 2537);
    auto source = at(ctx, 2530, -1);
    auto frog   = at(ctx, 2537, 0, 2);
    ctx.require(co_await ctx.ensureLoaded(b), "test area loaded");
    ctx.require(ctx.setBlock(a, pk::kChainConveyorId) && ctx.setBlock(b, pk::kChainConveyorId), "conveyors placed");
    ctx.require(ctx.setBlock(source, "levitate:kinetics_test_source", {{"levitate:axis", 1}}), "a rotation source below");
    pk::track(a, pk::Kind::ChainConveyor);
    pk::track(b, pk::Kind::ChainConveyor);
    co_await ctx.ticks(2);
    ctx.require(pk::connectConveyors(nullptr, a, b), "connected");
    lv::feature::kinetics::setGeneratedSpeed(source, 128);
    co_await ctx.ticks(3);
    ctx.require(lv::feature::kinetics::speedAt(b) != 0, "the far conveyor turns: " + std::to_string(lv::feature::kinetics::speedAt(b)));
    // a frogport catching "Depot" on the loop of b
    ctx.require(ctx.setBlock(frog, pk::kFrogportId), "frogport placed");
    pk::track(frog, pk::Kind::Frogport);
    auto& port    = pk::port(frog);
    port.address  = "Depot";
    port.target   = pk::PortTarget{pk::PortTarget::Type::Chain, pk::diff(frog, b), 0.f, std::nullopt, false};
    pk::savePort(frog);
    pk::registerWithTarget(frog);
    // one box for the frogport, one for nobody
    auto box = pk::containing({stack("minecraft:lapis_lazuli", 3)});
    pk::setAddress(box, "Depot");
    pk::ChainPackage p;
    p.item  = box;
    p.netId = pk::nextNetId();
    ctx.require(pk::addLoopingPackage(a, p), "box on the first loop");
    bool caught = false;
    for (int i = 0; i < 40 && !caught; ++i) {
        co_await ctx.ticks(10);
        for (auto const& s : pk::port(frog).inventory) caught |= pk::isPackage(s) && pk::addressOf(s) == "Depot";
    }
    ctx.check(caught, "the frogport caught its package after travelling along the chain");
    lv::feature::kinetics::setGeneratedSpeed(source, 0);
    pk::deregisterFromTarget(frog);
    pk::disconnectConveyors(a, b, false, nullptr);
    co_await ctx.ticks(2);
});

LV_REGISTER_SELFTEST("packages.frogport_send", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto conv     = at(ctx, 2540);
    auto source   = at(ctx, 2540, -1);
    auto packager = at(ctx, 2542);
    auto frog     = at(ctx, 2542, 1);
    ctx.require(co_await ctx.ensureLoaded(conv), "test area loaded");
    ctx.require(ctx.setBlock(conv, pk::kChainConveyorId), "conveyor placed");
    ctx.require(ctx.setBlock(source, "levitate:kinetics_test_source", {{"levitate:axis", 1}}), "rotation source");
    pk::track(conv, pk::Kind::ChainConveyor);
    ctx.require(placePackager(ctx, packager, 1), "packager placed below the frogport");
    ctx.require(ctx.setBlock(frog, pk::kFrogportId), "frogport placed");
    pk::track(frog, pk::Kind::Frogport);
    co_await ctx.ticks(3);
    auto& port   = pk::port(frog);
    port.address = "Here";
    port.target  = pk::PortTarget{pk::PortTarget::Type::Chain, pk::diff(frog, conv), 90.f, std::nullopt, false};
    pk::savePort(frog);
    pk::registerWithTarget(frog);
    auto box = pk::containing({stack("minecraft:glass", 4)});
    pk::setAddress(box, "Elsewhere");
    pk::packager(packager).held = box;
    bool sent = false;
    for (int i = 0; i < 10 && !sent; ++i) {
        co_await ctx.ticks(10);
        sent = !pk::conveyor(conv).looping.empty();
    }
    ctx.check(sent, "the frogport took the package from the packager and hung it on the chain");
    ctx.check(items::isEmpty(pk::packager(packager).held), "the packager is empty");
    pk::conveyor(conv).looping.clear();
    pk::saveConveyor(conv);
    pk::deregisterFromTarget(frog);
});

LV_REGISTER_SELFTEST("packages.postbox", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto box     = at(ctx, 2550);
    auto station = at(ctx, 2553);
    ctx.require(co_await ctx.ensureLoaded(box), "test area loaded");
    ctx.require(ctx.setBlock(box, "levitate:red_postbox"), "postbox placed");
    pk::track(box, pk::Kind::Postbox);
    auto& port   = pk::port(box);
    port.address = "Home";
    port.target  = pk::PortTarget{pk::PortTarget::Type::Station, pk::diff(box, station), 0.f, std::nullopt, false};
    pk::savePort(box);
    pk::registerWithTarget(box);
    auto list = pk::postboxesForStation(station);
    ctx.require(list.size() == 1 && list[0].pos == box && list[0].address == "Home", "the station knows its postbox");
    auto* region = pk::regionOf(box.dim);
    auto  home   = pk::containing({stack("minecraft:cake", 1)});
    pk::setAddress(home, "Home");
    auto away = pk::containing({stack("minecraft:torch", 8)});
    pk::setAddress(away, "Mine");
    // automation: packages for elsewhere go in, packages for the port come out
    auto rest = lv::feature::common::items::insert(*region, lv::game::blockPos(box), 1, home, false);
    ctx.check(!items::isEmpty(rest), "automation can't insert a package addressed to the port");
    rest = lv::feature::common::items::insert(*region, lv::game::blockPos(box), 1, away, false);
    ctx.check(items::isEmpty(rest), "automation inserts packages for elsewhere");
    // a train delivers
    ctx.check(items::isEmpty(pk::insertIntoPort(box, home, false)), "a train unloads into the postbox");
    co_await ctx.ticks(1);
    ctx.check(pk::stateAt(box, pk::kFlag).value_or(0) == 1, "the flag is up");
    auto out = lv::feature::common::items::extract(*region, lv::game::blockPos(box), 0, items::any(), 1, false);
    ctx.check(pk::addressOf(out) == "Home", "automation extracts the delivered package");
    auto picked = pk::extractFromPort(box, [](ItemStack const& s) { return pk::addressOf(s) != "Home"; }, false);
    ctx.check(pk::addressOf(picked) == "Mine", "a train picks up the outgoing package");
    pk::savePort(box);
    co_await ctx.ticks(1);
    ctx.check(pk::stateAt(box, pk::kFlag).value_or(1) == 0, "the flag is down again");
});

LV_REGISTER_SELFTEST("packages.chain_package_pick", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto conv = at(ctx, 2560);
    ctx.require(co_await ctx.ensureLoaded(conv), "test area loaded");
    ctx.require(ctx.setBlock(conv, pk::kChainConveyorId), "conveyor placed");
    pk::track(conv, pk::Kind::ChainConveyor);
    co_await ctx.ticks(1);
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    // stand south of the conveyor, looking north at the loop (chain position 0 = south side)
    sp.teleport(Vec3{conv.x + 0.5f, conv.y - 1.2f, conv.z + 3.5f}, DimensionType(conv.dim));
    co_await ctx.ticks(2);
    sp.simulateLookAt(lv::game::blockPos(conv), sim::LookDuration::Instant);
    co_await ctx.ticks(2);
    auto pick = pk::pickChain(sp, false, 8);
    ctx.check(pick.has_value() && pick->conveyor == conv && !pick->connection, "the loop is under the crosshair");
    auto box = pk::containing({stack("minecraft:clay_ball", 2)});
    sp.simulateSetItem(box, true, 0);
    co_await ctx.ticks(1);
    if (pick) pk::putPackageOnChain(sp, *pick);
    ctx.check(pk::conveyor(conv).looping.size() == 1, "the package hangs on the loop");
    co_await ctx.ticks(1);
    auto pp = pk::pickPackage(sp, 8);
    ctx.check(pp.has_value(), "the hanging package can be picked");
    if (pp) pk::takePackageFromChain(sp, *pp);
    ctx.check(pk::conveyor(conv).looping.empty(), "taken off the chain");
});
