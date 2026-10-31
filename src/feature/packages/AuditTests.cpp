// Selftests of the U56 package fixes (testPos dx 5685..5699; row 56 lies outside the kept test
// area, so these run at z spawn - 60 like the stock part of U56).

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/phys/AABB.h"

#include "feature/common/Items.h"
#include "feature/display/Clipboard.h"
#include "feature/goggles/GoggleInfo.h"
#include "feature/net/Net.h"
#include "feature/packages/Packages.h"
#include "feature/packages/PackagesInternal.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::selftest::Context;
namespace pk    = lv::feature::packages;
namespace items = lv::feature::common::items;
namespace net   = lv::feature::net;
using lv::core::GlobalPos;
using JsonT = nlohmann::json;

constexpr int kU56Dz = -295;

GlobalPos at(Context& ctx, int dx, int dy = 1) {
    auto p = ctx.testPos(dx, kU56Dz);
    p.y += dy;
    return p;
}

ItemStack stack(char const* id, int count = 1) { return items::make(id, count).value_or(ItemStack::EMPTY_ITEM()); }

std::vector<Actor*> packageEntities(GlobalPos const& p, float r = 3.f) {
    std::vector<Actor*> out;
    auto*               region = pk::regionOf(p.dim);
    if (!region) return out;
    AABB box{Vec3{p.x - r, p.y - 2.f, p.z - r}, Vec3{p.x + 1 + r, p.y + 4.f, p.z + 1 + r}};
    for (auto* a : region->getEntities(box, 0.f))
        if (a && a->isAlive() && a->getTypeName() == pk::kPackageEntityId) out.push_back(a);
    return out;
}

void removeAround(GlobalPos const& p) {
    for (auto* a : packageEntities(p, 4.f)) a->remove();
}

} // namespace

LV_REGISTER_SELFTEST("packages.frogport_yaw_hint", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto frog = at(ctx, 5685);
    ctx.require(co_await ctx.ensureLoaded(frog), "test area loaded");
    ctx.require(ctx.setBlock(frog, pk::kFrogportId), "frogport placed");
    pk::track(frog, pk::Kind::Frogport);
    // Create's getYaw without a target: the yaw it was placed with, in 22.5 degree steps
    pk::port(frog).passiveYaw = 90.f;
    pk::updateFrogYaw(frog);
    ctx.check(pk::stateAt(frog, pk::kYaw).value_or(-1) == 4, "a frog placed at yaw 90 shows step 4");
    pk::port(frog).passiveYaw = -45.f;
    pk::updateFrogYaw(frog);
    ctx.check(pk::stateAt(frog, pk::kYaw).value_or(-1) == 14, "yaw -45 is step 14");
    // Create's addToTooltip: "Contains Packages" when the inventory below refused a package
    auto& p = ctx.player();
    pk::port(frog).failedLastExport = true;
    bool hint = false;
    for (auto const& l : lv::feature::goggles::overlayFor(p, frog).lines) hint = hint || l.find("Contains Packages") != std::string::npos;
    ctx.check(hint, "the blocked frogport shows Create's hint");
    pk::port(frog).failedLastExport = false;
    co_await ctx.ticks(1);
});

LV_REGISTER_SELFTEST("packages.addresses", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's AddressEditBox: 25 characters; AddressEditBoxHelper: "#address" clipboard entries
    auto box = at(ctx, 5687);
    ctx.require(co_await ctx.ensureLoaded(box), "test area loaded");
    ctx.require(ctx.setBlock(box, "levitate:green_postbox"), "postbox placed");
    pk::track(box, pk::Kind::Postbox);
    auto& p = ctx.player();
    net::setTestCompanion(p, true);
    net::injectFromClient(p, "screen.action", {{"type", "packages.port"}, {"context", {{"pos", JsonT::array({box.dim, box.x, box.y, box.z})}}},
                                              {"action", {{"op", "address"}, {"text", std::string(30, 'x')}}}});
    net::setTestCompanion(p, false);
    (void)net::takeTestMessages(p);
    ctx.check(pk::port(box).address.size() == 25, "a port address is cut to 25 characters, got " + std::to_string(pk::port(box).address.size()));
    namespace cb = lv::feature::display::clipboard;
    auto clip    = cb::makeWritten({{cb::Entry{"#Storage", false, nullptr, 0}, cb::Entry{"#Done", true, nullptr, 0}, cb::Entry{"note", false, nullptr, 0}}}, "Addresses");
    ctx.require(clip.has_value(), "a clipboard");
    p.setSelectedItem(*clip);
    auto s = pk::addressSuggestions(p, "Local");
    ctx.check(s.size() == 2 && s[0] == "Local" && s[1] == "Storage", "suggestions: the local address, then unchecked '#' entries of held clipboards");
    p.setSelectedItem(items::empty());
});

LV_REGISTER_SELFTEST("packages.postbox_offline", [](Context& ctx) {
    // Create's GlobalPackagePort: a station keeps serving a postbox whose chunk is unloaded
    auto station = ctx.testPos(5689, 8000); // far away: not loaded
    auto box     = GlobalPos{station.dim, station.x + 1, station.y, station.z};
    pk::addStationPortRecord(box, station, "Far");
    auto list = pk::postboxesForStation(station);
    ctx.require(list.size() == 1 && list[0].pos == box && list[0].address == "Far", "the station lists its unloaded postbox");
    auto mail = pk::containing({stack("minecraft:paper", 3)});
    pk::setAddress(mail, "Far");
    ctx.check(items::isEmpty(pk::insertIntoPort(box, mail, false)), "a train delivers into the offline buffer");
    auto inv = pk::portInventory(box);
    ctx.check(!inv.empty() && pk::isPackage(inv[0]), "the buffer holds the package");
    auto out = pk::extractFromPort(box, {}, false);
    ctx.check(pk::addressOf(out) == "Far", "a train can take it back out");
});

LV_REGISTER_SELFTEST("packages.tray_and_vanilla_items", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's getRenderedBox: the box stays on the tray while held; vanilla clients get it as a
    // client-only item
    auto packager = at(ctx, 5691);
    ctx.require(co_await ctx.ensureLoaded(packager), "test area loaded");
    ctx.require(ctx.setBlock(packager, pk::kPackagerId, {{pk::kFacing, 1}}), "packager placed");
    pk::track(packager, pk::Kind::Packager);
    pk::onPackagerPlaced(packager, 1);
    pk::packager(packager).held = pk::containing({stack("minecraft:stick", 2)});
    co_await ctx.ticks(2);
    ctx.check(pk::vanillaItemCount("tray:") >= 1, "the held box is shown to vanilla clients");
    pk::packager(packager).held = items::empty();
    co_await ctx.ticks(2);
    ctx.check(pk::vanillaItemCount("tray:" + packager.key()) == 0, "taken: gone");
    pk::showVanillaItem("test:a", packager.dim, stack("minecraft:apple"), {packager.x + .5, packager.y + 2.0, packager.z + .5});
    ctx.check(pk::vanillaItemCount("test:") == 1, "an item for vanilla clients");
    pk::hideVanillaItems("test:");
    ctx.check(pk::vanillaItemCount("test:") == 0, "hidden by prefix");
});

LV_REGISTER_SELFTEST("packages.entity_pile", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's PackageEntity.canCollideWith: a package lands on the package below it
    auto pos = at(ctx, 5693, 0);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    removeAround(pos);
    ctx.require(ctx.setBlock({pos.dim, pos.x, pos.y - 1, pos.z}, "minecraft:stone"), "floor");
    ctx.cleanupBlock(pos);
    auto lower = pk::spawnPackageEntity(pos.dim, {pos.x + .5, double(pos.y), pos.z + .5}, pk::containing({stack("minecraft:stick", 1)}));
    co_await ctx.ticks(10);
    auto upper = pk::spawnPackageEntity(pos.dim, {pos.x + .5, pos.y + 1.6, pos.z + .5}, pk::containing({stack("minecraft:stick", 1)}));
    ctx.require(lower && upper, "two package entities");
    co_await ctx.ticks(30);
    auto found = packageEntities(pos);
    ctx.require(found.size() == 2, "both are there");
    float lo = std::min(found[0]->getAABB().min.y, found[1]->getAABB().min.y);
    float hi = std::max(found[0]->getAABB().min.y, found[1]->getAABB().min.y);
    float lowerTop = found[0]->getAABB().min.y < found[1]->getAABB().min.y ? found[0]->getAABB().max.y : found[1]->getAABB().max.y;
    ctx.check(std::abs(lo - pos.y) < .1f, "the lower package stands on the ground");
    ctx.check(std::abs(hi - lowerTop) < .1f, "the upper package rests on the lower one, got " + std::to_string(hi) + " on " + std::to_string(lowerTop));
    removeAround(pos);
});

LV_REGISTER_SELFTEST("packages.entity_suffocate", [](Context& ctx) -> ll::coro::CoroTask<> {
    // Create's PackageEntity.hurt(inWall): a package inside a solid block breaks open
    auto pos = at(ctx, 5696, 0);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    removeAround(pos);
    ctx.require(ctx.setBlock(pos, "minecraft:stone"), "stone");
    ctx.require(ctx.setBlock({pos.dim, pos.x, pos.y + 1, pos.z}, "minecraft:stone"), "stone above");
    auto id = pk::spawnPackageEntity(pos.dim, {pos.x + .5, pos.y + .2, pos.z + .5}, pk::containing({stack("minecraft:flint", 2)}));
    ctx.require(id != 0, "package entity spawned in the stone");
    co_await ctx.ticks(4);
    ctx.check(!pk::packageEntityBox(id), "the package broke open");
    removeAround(pos);
});
