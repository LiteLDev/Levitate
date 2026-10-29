// Selftests of the wave-1 integration (U32, testPos dx 3200..3299): the kinetics and tracks sync
// over the companion protocol, kinetic records in BlockData (and their migration), BlockData on
// contraptions, the chassis value box, the shared interaction dispatch and the display entities'
// crash safety.

#include <algorithm>
#include <string>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

#include "core/common/Transfer.h"
#include "core/kinetics/SyncCodec.h"
#include "feature/common/BlockData.h"
#include "feature/common/Interactions.h"
#include "feature/common/Items.h"
#include "feature/common/Redstone.h"
#include "feature/common/ValueBox.h"
#include "feature/contraption/Contraption.h"
#include "feature/contraption/Internal.h"
#include "feature/kinetics/Kinetics.h"
#include "feature/kinetics/KineticsInternal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "selftest/SelfTest.h"

namespace lv::feature::integration {

namespace {

using lv::selftest::Context;
using GP = core::GlobalPos;
namespace bd = common::blockdata;
namespace ia = common::interactions;
namespace ks = core::kinetics::sync;
namespace kn = feature::kinetics;
namespace ct = feature::contraption;

constexpr char kSource[] = "levitate:kinetics_test_source";
constexpr char kShaft[]  = "levitate:kinetics_test_shaft";
constexpr char kLinearChassis[] = "levitate:linear_chassis";

GP at(GP p, int dx, int dy = 0, int dz = 0) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }

/// Every batch of the "kinetics.sync" messages recorded for a test companion.
std::vector<ks::Batch> syncBatches(Player const& p) {
    std::vector<ks::Batch> out;
    for (auto const& m : net::takeTestMessages(p)) {
        if (m.channel != kn::detail::kSyncChannel) continue;
        for (auto const& t : m.payload.value("batches", nlohmann::json::array())) {
            if (auto b = ks::decode(t.get<std::string>())) out.push_back(*b);
        }
    }
    return out;
}

std::optional<float> speedIn(std::vector<ks::Batch> const& batches, GP const& p) {
    std::optional<float> out;
    for (auto const& b : batches) {
        if (b.dim != p.dim) continue;
        for (auto const& e : b.entries)
            if (e.x == p.x && e.y == p.y && e.z == p.z) out = e.removed ? std::nullopt : std::optional<float>(e.speed);
    }
    return out;
}

/// Unmarks the test companion even when a check aborts the test.
struct CompanionGuard {
    Player& player;
    ~CompanionGuard() { net::setTestCompanion(player, false); }
};

struct ContraptionGuard {
    std::optional<ct::Id> id;
    ~ContraptionGuard() {
        if (id && ct::find(*id)) ct::disassemble(*id);
    }
};

} // namespace

LV_REGISTER_SELFTEST("integration.kinetics_sync", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3200);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& player = ctx.player();
    ctx.setBlock(p, kSource, {{"levitate:axis", 0}});
    ctx.setBlock(at(p, 1), kShaft, {{"levitate:axis", 0}});
    co_await ctx.ticks(2);
    // a vanilla client gets nothing
    ctx.check(!net::send(player, kn::detail::kSyncChannel, {}), "nothing is sent to a non-companion");

    net::joinTestCompanion(player);
    CompanionGuard guard{player};
    co_await ctx.ticks(2);
    auto full = syncBatches(player);
    ctx.check(!full.empty() && full.front().reset, "the handshake brings a full sync starting with a reset");
    ctx.check(speedIn(full, at(p, 1)) == 16.f, "the full sync carries the shaft's speed");

    kn::setGeneratedSpeed(p, 32);
    co_await ctx.ticks(2);
    auto delta = syncBatches(player);
    ctx.check(!delta.empty() && !delta.front().reset, "changes come as deltas");
    ctx.check(speedIn(delta, at(p, 1)) == 32.f, "the delta carries the new speed");

    ctx.setBlock(at(p, 1), "minecraft:air");
    co_await ctx.ticks(2);
    auto removed = syncBatches(player);
    bool gone    = std::any_of(removed.begin(), removed.end(), [&](ks::Batch const& b) {
        return std::any_of(b.entries.begin(), b.entries.end(), [&](ks::Entry const& e) {
            return e.removed && e.x == p.x + 1 && e.y == p.y && e.z == p.z;
        });
    });
    ctx.check(gone, "a broken shaft is synced as removed");
});

LV_REGISTER_SELFTEST("integration.sequence_context", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3280);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, kSource, {{"levitate:axis", 0}});
    ctx.setBlock(at(p, 1), kShaft, {{"levitate:axis", 0}});
    ctx.setBlock(at(p, 2), kShaft, {{"levitate:axis", 0}});
    co_await ctx.ticks(2);
    using SC = kn::SequenceContext;
    kn::setSequenceContext(at(p, 1), SC{SC::Kind::TurnAngle, 90});
    auto down = kn::sequenceContextAt(at(p, 2));
    ctx.check(down && down->kind == SC::Kind::TurnAngle && down->value == 90, "the shaft it drives gets the instruction");
    ctx.check(!kn::sequenceContextAt(p), "the generator upstream does not");
    kn::setGeneratedSpeed(p, 32);
    auto same = kn::sequenceContextAt(at(p, 2));
    ctx.check(same && same->value == 90, "a 1:1 chain keeps the value at any speed");
    kn::setSequenceContext(at(p, 1), std::nullopt);
    ctx.check(!kn::sequenceContextAt(at(p, 2)), "cleared");
});

LV_REGISTER_SELFTEST("integration.tracks_sync", [](Context& ctx) {
    auto& player = ctx.player();
    net::joinTestCompanion(player);
    CompanionGuard guard{player};
    bool reset = false;
    for (auto const& m : net::takeTestMessages(player))
        if (m.channel == "tracks.curves" && m.payload.value("op", "") == "reset") reset = true;
    ctx.check(reset, "the handshake brings the curve list (reset first)");
});

LV_REGISTER_SELFTEST("integration.kinetics_blockdata", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3210);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, kSource, {{"levitate:axis", 0}});
    ctx.setBlock(at(p, 1), kShaft, {{"levitate:axis", 0}});
    co_await ctx.ticks(2);
    kn::setStressImpact(at(p, 1), 3.0);
    kn::detail::saveNow();
    auto const* doc = bd::get(at(p, 1));
    ctx.require(doc && doc->contains("kinetics"), "the shaft's record is in its BlockData document");
    ctx.check((*doc)["kinetics"].value("impact", 0.0) == 3.0, "with its stress override");
    auto record = (*doc)["kinetics"];

    // a world saved by wave 1: the record in the old per-chunk key, nothing in BlockData
    bd::set(at(p, 1), nlohmann::json());
    auto legacyKey = "lv:kinetics:c:" + std::to_string(p.dim) + ":" + std::to_string(ks::chunkOf(p.x + 1)) + ":"
                   + std::to_string(ks::chunkOf(p.z));
    auto legacy = game::db::load(legacyKey).value_or(nlohmann::json::array());
    legacy.push_back(record);
    game::db::save(legacyKey, legacy);
    bd::flush();
    kn::detail::reloadFromDb();
    ctx.check(!game::db::load(legacyKey), "the old key is gone after the migration");
    auto const* migrated = bd::get(at(p, 1));
    ctx.check(migrated && migrated->contains("kinetics"), "the record moved into BlockData");
    auto n = kn::nodeAt(at(p, 1));
    ctx.check(n && n->speed == 16, "the shaft turns after the migration");
    ctx.check(n && n->impact == 3.0, "the override survives the migration");

    // breaking the block drops the record
    ctx.setBlock(at(p, 1), "minecraft:air");
    co_await ctx.ticks(2);
    kn::detail::saveNow();
    ctx.check(!bd::has(at(p, 1)), "no document after the block broke");
});

LV_REGISTER_SELFTEST("integration.contraption_blockdata", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3220);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kSource, {{"levitate:axis", 0}}), "source placed");
    ctx.setBlock(at(p, 1), kShaft, {{"levitate:axis", 0}});
    ctx.setBlock(at(p, 2), "minecraft:stone");
    co_await ctx.ticks(2);
    kn::setGeneratedSpeed(p, 24);
    kn::setStressImpact(at(p, 1), 2.0);
    bd::edit(at(p, 2))["integration"] = "carried"; // any unit's data
    ct::addGlue(p.dim, core::contraption::GlueBox::of({p.x, p.y, p.z}, {p.x + 2, p.y, p.z}));

    ContraptionGuard g;
    ct::AssembleOptions o;
    o.anchor        = p;
    o.includeAnchor = true;
    auto r          = ct::assemble(o);
    ctx.require(r.ok(), "assembled: " + r.errorText);
    g.id = r.id;
    ctx.check(!bd::has(at(p, 2)) && !kn::nodeAt(at(p, 1)), "the data left with the blocks");

    // crash safety: the display entities are never saved with the world
    co_await ctx.ticks(2);
    auto* c = ct::find(*r.id);
    ctx.require(c && !c->blocks().empty(), "contraption alive");
    ActorUniqueID uid;
    uid.rawID   = c->blocks().front().display;
    auto* actor = ll::service::getLevel()->fetchEntity(uid, false);
    ctx.require(actor != nullptr, "display entity alive");
    CompoundTag tag;
    ctx.check(!actor->save(tag), "a display entity refuses to be saved");

    if (auto* cc = ct::find(*r.id)) {
        ct::Motion m;
        m.velocity = {0, 0.5, 0};
        m.ticks    = 4;
        cc->setMotion(m);
    }
    co_await ctx.ticks(7);
    for (int dx = 0; dx <= 2; ++dx) ctx.cleanupBlock(at(p, dx, 2));
    ctx.require(ct::disassemble(*r.id), "disassembled");
    g.id.reset();
    ct::clearGlue(p.dim, {p.x, p.y + 2, p.z}, 4);
    co_await ctx.ticks(2);

    auto const* doc = bd::get(at(p, 2, 2));
    ctx.check(doc && doc->value("integration", "") == "carried", "another unit's data landed with its block");
    auto src = kn::nodeAt(at(p, 0, 2));
    ctx.check(src && src->generatedSpeed == 24, "the generator kept its speed");
    auto shaft = kn::nodeAt(at(p, 1, 2));
    ctx.check(shaft && shaft->impact == 2.0, "the shaft kept its stress override");
    ctx.check(shaft && shaft->speed == 24, "and turns again at the generator's speed");
});

LV_REGISTER_SELFTEST("integration.chassis_value_box", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(3240);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.require(ctx.setBlock(p, kLinearChassis, {{"levitate:axis", 1}}), "chassis placed");
    ctx.setBlock(at(p, 0, 1), kLinearChassis, {{"levitate:axis", 1}});
    auto& player = ctx.player();
    auto& region = player.getDimensionBlockSource();
    auto const& block = region.getBlock(game::blockPos(p));
    auto const* box = common::valuebox::boxAt(block, 5, {1.0, 0.5, 0.5}); // centre of the east face
    ctx.require(box && box->id == "range", "the chassis has a range box on its faces");
    ctx.check(box->requiresWrench, "edited with the wrench only (as Create)");
    ctx.check(common::valuebox::apply(player, p, "range", {0, 5}), "the box accepts a value");
    ctx.check(ct::chassisRange(p) == 5 && ct::chassisRange(at(p, 0, 1)) == 5, "the connected group takes the range");

    // the wrench opens it through the shared dispatch; an empty hand does not
    net::setTestCompanion(player, true);
    CompanionGuard guard{player};
    std::string blockId = block.getTypeName(), wrench = ia::kWrenchId, none;
    ia::UseOnContext withWrench{player, common::items::empty(), wrench, block, blockId, p, 5, {1.0, 0.5, 0.5}, false, true};
    ctx.check(ia::dispatchUseOn(withWrench) == ia::Result::Consume, "a wrench click on the box is taken");
    co_await ctx.ticks(2);
    auto msgs   = net::takeTestMessages(player);
    bool opened = std::any_of(msgs.begin(), msgs.end(), [](auto const& m) { return m.channel == "valuebox.open"; });
    ctx.check(opened, "and opens the board");
    co_await ctx.ticks(6); // past the click debounce
    ia::UseOnContext empty{player, common::items::empty(), none, block, blockId, p, 5, {1.0, 0.5, 0.5}, false, true};
    ctx.check(ia::dispatchUseOn(empty) == ia::Result::Pass, "an empty hand does not open it");
});

LV_REGISTER_SELFTEST("integration.interactions", [](Context& ctx) {
    auto& player = ctx.player();
    auto  p      = ctx.testPos(3260);
    std::vector<std::string> calls;
    std::string blockId = "minecraft:stone", item = "minecraft:stick";
    auto low  = ia::onUseOn({.block = blockId}, [&](ia::UseOnContext const&) { calls.push_back("low"); return ia::Result::Consume; }, ia::priority::Low);
    auto high = ia::onUseOn({.block = blockId, .item = item}, [&](ia::UseOnContext const&) { calls.push_back("high"); return ia::Result::Pass; }, ia::priority::High);
    auto other = ia::onUseOn({.block = "minecraft:dirt"}, [&](ia::UseOnContext const&) { calls.push_back("other"); return ia::Result::Consume; });
    auto& region = player.getDimensionBlockSource();
    auto const& stone = *game::blockWith(blockId);
    ia::UseOnContext c{player, common::items::empty(), item, stone, blockId, p, 1, {0.5, 1.0, 0.5}, false, true};
    auto r = ia::dispatchUseOn(c);
    (void)region;
    ia::remove(low);
    ia::remove(high);
    ia::remove(other);
    ctx.check(r == ia::Result::Consume, "the low priority handler took it");
    ctx.check(calls == std::vector<std::string>{"high", "low"}, "handlers run by priority, matching ones only");
    calls.clear();
    ctx.check(ia::dispatchUseOn(c) == ia::Result::Pass && calls.empty(), "removed handlers are gone");
});

LV_REGISTER_SELFTEST("integration.redstone", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace rs = common::redstone;
    auto p = ctx.testPos(3270);
    p.y += 1;
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& region = ctx.player().getDimensionBlockSource();
    auto  bp     = game::blockPos(p);
    ctx.setBlock(p, "minecraft:stone");
    int  seen  = -1;
    auto watch = rs::watch(p, [&](GP const&, int power) { seen = power; });
    co_await ctx.ticks(3);
    ctx.check(rs::power(region, bp) == 0 && seen == 0, "nothing powers the block at first");

    ctx.setBlock(at(p, 1), "minecraft:redstone_block");
    co_await ctx.ticks(3);
    ctx.check(rs::power(region, bp) == 15, "a redstone block next to it: 15, got " + std::to_string(rs::power(region, bp)));
    ctx.check(seen == 15, "the watch saw the change");

    // wire next to it (on a floor), fed by a redstone block: the wire's signal
    ctx.setBlock(at(p, 1), "minecraft:stone");
    ctx.setBlock(at(p, 0, -1, 1), "minecraft:stone");
    ctx.setBlock(at(p, 0, -1, 2), "minecraft:stone");
    ctx.setBlock(at(p, 0, 0, 1), "minecraft:redstone_wire");
    ctx.setBlock(at(p, 0, 0, 2), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    int wire = rs::power(region, bp);
    ctx.check(wire >= 14, "a powered wire reaches it: " + std::to_string(wire));

    // a line of dust running past it (along x) does not point into it
    ctx.setBlock(at(p, 0, 0, 2), "minecraft:air");
    for (int dx = -1; dx <= 2; ++dx) ctx.setBlock(at(p, dx, -1, 1), "minecraft:stone");
    ctx.setBlock(at(p, -1, 0, 1), "minecraft:redstone_wire");
    ctx.setBlock(at(p, 1, 0, 1), "minecraft:redstone_wire");
    ctx.setBlock(at(p, 2, 0, 1), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    int past = rs::power(region, bp);
    ctx.check(past == 0, "dust running past it does not power it: " + std::to_string(past));
    ctx.setBlock(at(p, -1, 0, 1), "minecraft:air");
    ctx.setBlock(at(p, 1, 0, 1), "minecraft:air");
    ctx.setBlock(at(p, 2, 0, 1), "minecraft:air");

    // a conducting block: wire on top of the stone east of it powers that stone
    ctx.setBlock(at(p, 0, 0, 1), "minecraft:air");
    ctx.setBlock(at(p, 0, 0, 2), "minecraft:air");
    co_await ctx.ticks(3);
    ctx.check(rs::power(region, bp) == 0, "unpowered again: " + std::to_string(rs::power(region, bp)));
    ctx.setBlock(at(p, 2), "minecraft:stone");
    ctx.setBlock(at(p, 1, 1), "minecraft:redstone_wire");
    ctx.setBlock(at(p, 2, 1), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    int conducted = rs::power(region, bp);
    ctx.info("power through a conducting block: " + std::to_string(conducted));
    ctx.check(conducted > 0, "a block powered by wire passes the power on");
    // repeaters only power the block in front: compare with a vanilla lamp in the same spot
    ctx.setBlock(at(p, 1, 1), "minecraft:air");
    ctx.setBlock(at(p, 2, 1), "minecraft:air");
    ctx.setBlock(at(p, 2), "minecraft:air");
    for (auto const* facing : {"east", "west"}) {
        ctx.setBlock(at(p, 0, 0, 3), "minecraft:redstone_lamp");
        ctx.setBlock(at(p, 1, -1, 3), "minecraft:stone");
        auto const* repeater = game::blockWith("minecraft:unpowered_repeater");
        ctx.require(repeater != nullptr, "repeater block");
        auto const* turned = ct::detail::withStates(*repeater, {{"minecraft:cardinal_direction", std::string(facing)}});
        ctx.cleanupBlock(at(p, 1, 0, 3));
        game::setBlock(region, game::blockPos(at(p, 1, 0, 3)), turned ? *turned : *repeater);
        ctx.setBlock(at(p, 2, 0, 3), "minecraft:redstone_block");
        co_await ctx.ticks(8);
        bool lit = game::blockIdAt(region, game::blockPos(at(p, 0, 0, 3))) == "minecraft:lit_redstone_lamp";
        int  pw  = rs::power(region, game::blockPos(at(p, 0, 0, 3)));
        ctx.info(std::string("repeater ") + facing + ": lamp " + (lit ? "lit" : "dark") + ", power " + std::to_string(pw));
        ctx.check(lit == (pw > 0), std::string("repeater facing ") + facing + " agrees with the vanilla lamp");
        ctx.setBlock(at(p, 2, 0, 3), "minecraft:air");
        ctx.setBlock(at(p, 1, 0, 3), "minecraft:air");
        co_await ctx.ticks(4);
    }

    rs::unwatch(watch);
    int before = seen;
    ctx.setBlock(at(p, 2, 1), "minecraft:air");
    co_await ctx.ticks(3);
    ctx.check(seen == before, "an unwatched position is no longer reported");
});

} // namespace lv::feature::integration
