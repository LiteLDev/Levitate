// Selftests of the building unit's U43 additions (testPos dx 4350..4399): pane / window connected
// textures, copycat material turning, the girder placement helper, sliding doors closing,
// bracket orientation and the client sync of copycats and brackets.

#include "mc/deps/core/math/Vec3.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "feature/building/Building.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace {

using lv::selftest::Context;
namespace b = lv::feature::building;
namespace net = lv::feature::net;
using lv::core::GlobalPos;

GlobalPos at(GlobalPos const& p, int dx, int dy, int dz) { return {p.dim, p.x + dx, p.y + dy, p.z + dz}; }
int       st(GlobalPos const& p, char const* name) {
    auto const* blk = b::blockAt(p);
    return blk ? b::stateOf(*blk, name, -1) : -1;
}
std::string materialState(GlobalPos const& p, std::string const& key) {
    auto const* m = b::copycatMaterialBlock(p);
    if (!m) return {};
    auto const& nbt = *m->mSerializationId;
    if (!nbt.contains("states") || !nbt["states"].contains(key)) return {};
    auto s = nbt["states"][key].toSnbt(SnbtFormat::Minimize, 0);
    std::erase(s, '"');
    return s;
}
int countOf(Player& p, std::string const& item) {
    int   n   = 0;
    auto& inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& it = inv.getItem(i);
        if (!it.isNull() && it.getTypeName() == item) n += it.mCount;
    }
    return n;
}
void stand(Player& sp, GlobalPos const& p) { sp.teleport(Vec3{p.x + .5f, float(p.y), p.z + .5f}, DimensionType(p.dim)); }

} // namespace

LV_REGISTER_SELFTEST("building.pane_connected_textures", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4350);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "levitate:framed_glass_pane");
    ctx.setBlock(at(p, 1, 0, 0), "levitate:framed_glass_pane");
    ctx.setBlock(at(p, 0, 1, 0), "levitate:framed_glass_pane");
    ctx.setBlock(at(p, -1, 0, 0), "levitate:tiled_glass_pane");
    co_await ctx.ticks(4);
    ctx.check(st(p, "levitate:c_east") == 1 && st(p, "levitate:c_up") == 1, "a pane joins the same pane beside and above");
    ctx.check(st(p, "levitate:c_west") == 0 && st(p, "levitate:west") == 1, "another pane: an arm but no join");
    ctx.check(st(at(p, 1, 0, 0), "levitate:c_west") == 1, "both ways");
    // windows: vertical joins only for wooden panes, rectangles for industrial iron
    ctx.setBlock(at(p, 0, 0, 3), "levitate:oak_window_pane");
    ctx.setBlock(at(p, 0, 1, 3), "levitate:oak_window_pane");
    ctx.setBlock(at(p, 0, 0, 5), "levitate:industrial_iron_window");
    ctx.setBlock(at(p, 1, 0, 5), "levitate:industrial_iron_window");
    co_await ctx.ticks(4);
    ctx.check(st(at(p, 0, 0, 3), "levitate:c_up") == 1, "window panes join vertically");
    ctx.check(st(at(p, 0, 0, 5), "levitate:c_east") == 1, "industrial iron windows join sideways (Create's RECTANGLE)");
});

LV_REGISTER_SELFTEST("building.copycat_turning", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4355);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = ctx.player();
    stand(sp, at(p, 2, 0, 0));
    ctx.setBlock(p, "levitate:copycat_step", {{"levitate:facing", b::North}});
    ctx.setBlock(at(p, 0, 0, 1), "levitate:copycat_step", {{"levitate:facing", b::North}});
    co_await ctx.ticks(2);
    ctx.give(sp, b::makeItem("minecraft:oak_log", 8), 0);
    ctx.require(b::useCopycat(sp, b::makeItem("minecraft:oak_log", 1), p, b::Up), "a log applies");
    ctx.check(materialState(p, "pillar_axis") == "y", "the log takes the clicked face's axis (" + materialState(p, "pillar_axis") + ")");
    ctx.check(b::useCopycat(sp, b::makeItem("minecraft:oak_log", 1), p, b::Up), "the same block again turns it");
    ctx.check(materialState(p, "pillar_axis") == "z", "y -> z (" + materialState(p, "pillar_axis") + ")");
    ctx.check(b::useCopycat(sp, b::makeItem("minecraft:oak_log", 1), p, b::Up) && materialState(p, "pillar_axis") == "x", "z -> x");
    // the neighbour of the same copycat state lends its orientation
    ctx.check(b::useCopycat(sp, b::makeItem("minecraft:oak_log", 1), at(p, 0, 0, 1), b::Up), "the neighbour takes a log");
    ctx.check(materialState(at(p, 0, 0, 1), "pillar_axis") == "x", "and the neighbour's orientation");
    // nothing to turn: the use passes
    ctx.setBlock(at(p, 0, 0, 3), "levitate:copycat_panel", {{"levitate:facing", b::Up}});
    co_await ctx.ticks(1);
    ctx.check(b::useCopycat(sp, b::makeItem("minecraft:oak_planks", 1), at(p, 0, 0, 3), b::Up), "planks apply");
    ctx.check(!b::useCopycat(sp, b::makeItem("minecraft:oak_planks", 1), at(p, 0, 0, 3), b::Up), "planks have nothing to turn");
    ctx.give(sp, ItemStack{}, 0);
});

LV_REGISTER_SELFTEST("building.girder_helper", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4360);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = ctx.player();
    stand(sp, at(p, 0, 0, 3));
    ctx.setBlock(p, b::kGirderId, {{"levitate:axis", 0}, {"levitate:x", 1}});
    ctx.cleanupBlock(at(p, 1, 0, 0));
    ctx.cleanupBlock(at(p, 2, 0, 0));
    ctx.cleanupBlock(at(p, -1, 0, 0));
    co_await ctx.ticks(2);
    auto girder = b::makeItem(b::kGirderId, 8);
    ctx.give(sp, girder, 0);
    sp.setSelectedSlot(0);
    ctx.check(b::onUse(sp, girder, p, b::South, Vec3{.95f, .5f, .5f}, true), "a girder on a girder line extends it");
    ctx.check(b::idAt(at(p, 1, 0, 0)) == b::kGirderId && st(at(p, 1, 0, 0), "levitate:x") == 1, "at the east end, along x");
    ctx.check(b::onUse(sp, girder, p, b::South, Vec3{.95f, .5f, .5f}, true) && b::idAt(at(p, 2, 0, 0)) == b::kGirderId, "past the girders already there");
    ctx.check(b::onUse(sp, girder, p, b::South, Vec3{.05f, .5f, .5f}, true) && b::idAt(at(p, -1, 0, 0)) == b::kGirderId, "the end nearest to the click first");
    ctx.check(b::idAt(at(p, 0, 1, 0)) != b::kGirderId, "never across the beam");
    ctx.give(sp, ItemStack{}, 0);
});

LV_REGISTER_SELFTEST("building.door_closing", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4365);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    ctx.setBlock(p, "levitate:train_door", {{"levitate:facing", b::North}});
    ctx.setBlock(at(p, 0, 1, 0), "levitate:train_door", {{"levitate:facing", b::North}, {"levitate:upper", 1}});
    co_await ctx.ticks(2);
    ctx.require(b::useDoor(nullptr, p), "the door opens");
    co_await ctx.ticks(2);
    ctx.check(st(p, "levitate:open") == 1 && st(p, "levitate:closing") == 0, "open");
    ctx.require(b::useDoor(nullptr, p), "the door closes");
    co_await ctx.ticks(1);
    ctx.check(st(p, "levitate:open") == 0 && st(p, "levitate:closing") == 1, "closing: the panel is still on its way");
    auto const* blk   = b::blockAt(p);
    auto        boxes = blk ? b::collisionBoxes(*b::infoOf(*blk), *blk, false, false) : std::nullopt;
    ctx.check(boxes && boxes->size() == 1 && (*boxes)[0][3] - (*boxes)[0][0] < 16.f, "the open collision stays while closing");
    co_await ctx.ticks(9);
    ctx.check(st(p, "levitate:closing") == 0 && st(at(p, 0, 1, 0), "levitate:closing") == 0, "closed once the panel is back");
});

LV_REGISTER_SELFTEST("building.bracket_orientation", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4370);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    if (!lv::game::blockWith(b::kShaftId)) {
        ctx.info("levitate:shaft not registered (U6): skipped");
        co_return;
    }
    auto& sp = ctx.player();
    stand(sp, at(p, 0, 0, 3));
    ctx.setBlock(p, b::kShaftId, {{"levitate:axis", 0}});
    co_await ctx.ticks(2);
    ctx.give(sp, b::makeItem("levitate:metal_bracket", 2), 0);
    ctx.check(b::useBracket(sp, b::makeItem("levitate:metal_bracket", 1), p, b::Up), "a bracket goes on");
    auto const* d = b::bracketData(p);
    ctx.require(d != nullptr, "the bracket is stored");
    ctx.check(d->value("facing", -1) == b::Up && d->value("axis", -1) == 0 && !d->value("cog", true), "facing the clicked face, along the shaft");
    ctx.give(sp, b::makeItem("levitate:wooden_bracket", 1), 0);
    int metal = countOf(sp, "levitate:metal_bracket");
    ctx.check(b::useBracket(sp, b::makeItem("levitate:wooden_bracket", 1), p, b::Down), "another bracket swaps it");
    ctx.check(b::bracketAt(p) == std::optional<std::string>("levitate:wooden_bracket"), "the wooden one is on");
    ctx.check(countOf(sp, "levitate:metal_bracket") == metal + 1, "the metal one is given back");
    ctx.check(b::removeBracket(&sp, p) && !b::bracketAt(p), "the wrench takes it off");
    ctx.give(sp, ItemStack{}, 0);
});

LV_REGISTER_SELFTEST("building.visual_sync", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = ctx.testPos(4375);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& sp = ctx.player();
    net::setTestCompanion(sp, true);
    (void)net::takeTestMessages(sp);
    ctx.setBlock(p, "levitate:copycat_panel", {{"levitate:facing", b::Up}});
    co_await ctx.ticks(2);
    ctx.require(b::useCopycat(sp, b::makeItem("minecraft:stone", 1), p, b::Up), "stone applies");
    bool pushed = false;
    for (auto const& m : net::takeTestMessages(sp)) {
        if (m.channel != b::kVisualsChannel) continue;
        for (auto const& e : m.payload.value("entries", nlohmann::json::array()))
            if (e.value("x", 0) == p.x && e.value("z", 0) == p.z && e.value("copycat", std::string()).find("minecraft:stone") != std::string::npos) pushed = true;
    }
    ctx.check(pushed, "the material is pushed to companion players");
    net::injectFromClient(sp, b::kQueryChannel, {{"chunks", nlohmann::json::array({nlohmann::json::array({p.x >> 4, p.z >> 4})})}});
    bool answered = false;
    for (auto const& m : net::takeTestMessages(sp)) {
        if (m.channel != b::kVisualsChannel || !m.payload.contains("chunks")) continue;
        for (auto const& e : m.payload.value("entries", nlohmann::json::array()))
            if (e.value("x", 0) == p.x && e.value("y", 0) == p.y && e.contains("copycat")) answered = true;
    }
    ctx.check(answered, "a chunk query lists the copycat");
    net::setTestCompanion(sp, false);
});
