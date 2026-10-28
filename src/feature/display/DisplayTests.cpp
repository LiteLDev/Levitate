// Selftests of the display unit (U24, testPos dx 2400..2499).

#include "mc/deps/core/math/Vec3.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/sim/LookDuration.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/PlayerInventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/dimension/DimensionType.h"
#include "mc/world/level/block/Block.h"

#include "core/common/ValueSettings.h"
#include "feature/common/BlockData.h"
#include "feature/common/Items.h"
#include "feature/common/ValueBox.h"
#include "feature/display/Internal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"
#include "selftest/SelfTest.h"

namespace lv::feature::display {
std::string signText(BlockSource& region, GlobalPos const& pos);
}

namespace {
using lv::selftest::Context;
using lv::core::GlobalPos;
using JsonT          = nlohmann::json;
namespace dsp        = lv::feature::display;
namespace items      = lv::feature::common::items;
namespace blockdata  = lv::feature::common::blockdata;
namespace net        = lv::feature::net;

GlobalPos up(GlobalPos p, int dy) {
    p.y += dy;
    return p;
}
GlobalPos at(GlobalPos p, int dx, int dy, int dz) {
    p.x += dx;
    p.y += dy;
    p.z += dz;
    return p;
}
BlockSource& region(Context& ctx) { return ctx.player().getDimensionBlockSource(); }

/// Places a display link at `pos` attached to the block on `face ^ 1` (block_face = `face`).
bool placeLink(Context& ctx, GlobalPos const& pos, std::string const& face, GlobalPos const& target) {
    if (!ctx.setBlock(pos, dsp::kLinkId)) return false;
    auto&       r = region(ctx);
    auto const& b = r.getBlock(lv::game::blockPos(pos));
    if (auto const* next = dsp::withStates(b, {{"minecraft:block_face", face}})) lv::game::setBlock(r, lv::game::blockPos(pos), *next);
    dsp::link::LinkData d;
    d.targetOffset = {target.x - pos.x, target.y - pos.y, target.z - pos.z};
    dsp::link::write(pos, d);
    dsp::link::track(pos);
    return true;
}

void placeBoards(Context& ctx, GlobalPos const& origin, int w, int h, int facing) {
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; ++y) ctx.setBlock(at(origin, x, y, 0), dsp::kBoardId, {{"levitate:facing", facing}});
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; ++y) dsp::board::updateColumns(region(ctx), at(origin, x, y, 0));
}

ItemStack clipboardWith(Context& ctx, std::vector<std::string> const& texts) {
    auto s = items::make(dsp::kClipboardId);
    ctx.require(s.has_value(), "levitate:clipboard exists");
    dsp::clipboard::Contents c;
    c.type = 2;
    c.pages.emplace_back();
    for (auto const& t : texts) c.pages[0].push_back({t, false});
    dsp::clipboard::setOnItem(*s, c);
    return *s;
}
} // namespace

LV_REGISTER_SELFTEST("display.format", [](Context& ctx) {
    ctx.check(dsp::formatValue(999, true) == "999", "999 stays");
    ctx.check(dsp::formatValue(1234, true) == "1.2k", "1234 -> 1.2k, got " + dsp::formatValue(1234, true));
    ctx.check(dsp::formatValue(1500000, true) == "1.5m", "1500000 -> 1.5m");
    ctx.check(dsp::formatValue(1234, false) == "1234", "full numbers");
    ctx.check(dsp::progressBar(0.5, 4) == "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x92\xE2\x96\x92", "half bar");
    ctx.check(dsp::board::maxChars(1) == 4 && dsp::board::maxChars(2) == 8 && dsp::board::maxChars(3) == 13 && dsp::board::maxChars(32) == 145,
              "Create's character counts per width");
    ctx.check(dsp::nixie::idFor(1) == "levitate:nixie_tube" && dsp::nixie::idFor(14) == "levitate:red_nixie_tube", "nixie ids");
    ctx.check(dsp::nixie::isNixie("levitate:light_blue_nixie_tube") && !dsp::nixie::isNixie("levitate:orange_nixie_tube"), "nixie id check");
    ctx.check(dsp::dyeOf("minecraft:red_dye") == 14 && !dsp::dyeOf("minecraft:ink_sac"), "dyes");
});

LV_REGISTER_SELFTEST("display.registry", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(2400), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& r = region(ctx);
    struct Case {
        char const* block;
        char const* source;
    };
    int i = 0;
    for (auto c : {Case{"minecraft:target", "redstone_power"}, Case{"minecraft:enchanting_table", "enchant_power"},
                   Case{"minecraft:command_block", "scoreboard"}, Case{"minecraft:respawn_anchor", "death_count"},
                   Case{"levitate:nixie_tube", "nixie_tube"}}) {
        auto q = at(p, 0, 0, 2 * i++);
        ctx.require(ctx.setBlock(q, c.block), std::string("place ") + c.block);
        auto list  = dsp::sourcesAt(r, q);
        bool found = false;
        for (auto const* s : list) found |= s->id == c.source;
        ctx.check(found, std::string(c.block) + " offers " + c.source);
    }
    ctx.check(dsp::sourcesAt(r, at(p, 0, 1, 0)).empty(), "air offers nothing");
    ctx.require(ctx.setBlock(at(p, 3, 0, 0), dsp::kBoardId), "place a board");
    auto const* t = dsp::targetAt(r, at(p, 3, 0, 0));
    ctx.check(t && t->id == "display_board", "the board is a display target");
    ctx.require(ctx.setBlock(at(p, 3, 0, 2), "minecraft:standing_sign"), "place a sign");
    auto const* sign = dsp::targetAt(r, at(p, 3, 0, 2));
    ctx.check(sign && sign->id == "sign", "a sign is a display target");
    ctx.check(dsp::targetAt(r, at(p, 3, 0, 4)) == nullptr, "air is no target");
});

LV_REGISTER_SELFTEST("display.board_structure", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(2410), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& r = region(ctx);
    placeBoards(ctx, p, 3, 2, 0); // facing south: the controller is the west end of the top row
    auto s = dsp::board::structureOf(r, at(p, 2, 0, 0));
    ctx.require(s.has_value(), "structure found");
    ctx.check(s->controller == at(p, 0, 1, 0), "controller at the top west end, got " + s->controller.key());
    ctx.check(s->width == 3 && s->height == 2, "3x2, got " + std::to_string(s->width) + "x" + std::to_string(s->height));
    auto const& top = r.getBlock(lv::game::blockPos(at(p, 1, 1, 0)));
    ctx.check(dsp::intState(top, "levitate:down").value_or(0) == 1 && dsp::intState(top, "levitate:up").value_or(0) == 0, "top row connects down");
    // a fourth block on one row only splits the rows
    ctx.setBlock(at(p, 3, 0, 0), dsp::kBoardId, {{"levitate:facing", 0}});
    dsp::board::updateColumns(r, at(p, 3, 0, 0));
    auto s2 = dsp::board::structureOf(r, at(p, 0, 0, 0));
    ctx.check(s2 && s2->height == 1 && s2->width == 4, "uneven rows don't connect");
    co_return;
});

LV_REGISTER_SELFTEST("display.board_lines", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(2420), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& r = region(ctx);
    placeBoards(ctx, p, 2, 1, 0);
    auto s = dsp::board::structureOf(r, p);
    ctx.require(s.has_value(), "structure");
    dsp::board::setLine(r, s->controller, 0, "Hello, world!", true);
    ctx.check(dsp::board::lineText(s->controller, 0) == "Hello, w", "line truncated to 8 characters, got " + dsp::board::lineText(s->controller, 0));
    // a clipboard writes its entries from the clicked line down
    auto clip = clipboardWith(ctx, {"AB", "CD"});
    auto& pl  = ctx.player();
    bool used = dsp::board::use(pl, r, s->controller, 3, Vec3{0.5f, 0.75f, 1.0f}, clip);
    ctx.check(used, "clipboard used on the board");
    ctx.check(dsp::board::lineText(s->controller, 0) == "AB" && dsp::board::lineText(s->controller, 1) == "CD", "clipboard lines written");
    // dye and clearing need the board to turn (Create: medium speed)
    auto dye = items::make("minecraft:red_dye");
    ctx.check(!dsp::board::use(pl, r, s->controller, 3, Vec3{0.5f, 0.75f, 1.0f}, *dye), "a stopped board refuses dye");
    ctx.check(!dsp::board::running(s->controller), "not running without rotation");
    auto st = dsp::board::state(s->controller);
    ctx.check(st.size() == 2 && st[0].value("m", false), "two manual lines");
});

LV_REGISTER_SELFTEST("display.nixie_row", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(2430), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& r = region(ctx);
    // floor tubes facing south: the row runs north (left) to south
    for (int z = 0; z < 3; ++z) {
        ctx.setBlock(at(p, 0, 0, z), dsp::kNixieId, {{"levitate:facing", 0}, {"levitate:face", 0}});
        dsp::nixie::track(at(p, 0, 0, z));
    }
    auto row = dsp::nixie::row(r, at(p, 0, 0, 1));
    ctx.require(row.size() == 3 && row.front() == p, "row of three from the north end");
    dsp::nixie::displayCustomText(r, at(p, 0, 0, 2), "ABCDE");
    ctx.check(dsp::nixie::displayed(r, at(p, 0, 0, 0)) == "AB", "first tube AB");
    ctx.check(dsp::nixie::displayed(r, at(p, 0, 0, 1)) == "CD", "second tube CD");
    ctx.check(dsp::nixie::displayed(r, at(p, 0, 0, 2)) == "E ", "third tube 'E '");
    ctx.check(dsp::nixie::fullText(r, at(p, 0, 0, 1)) == "ABCDE", "full text");
    // dye recolours the whole row and keeps the text
    auto dye = items::make("minecraft:lime_dye");
    ctx.check(dsp::nixie::use(ctx.player(), r, at(p, 0, 0, 1), *dye), "dye used");
    ctx.check(lv::game::blockIdAt(r, lv::game::blockPos(at(p, 0, 0, 2))) == "levitate:lime_nixie_tube", "row dyed lime");
    ctx.check(dsp::nixie::displayed(r, at(p, 0, 0, 2)) == "E ", "text kept");
    // redstone: a tube without text shows the signal strength
    auto q = at(p, 3, 0, 0);
    ctx.setBlock(q, dsp::kNixieId, {{"levitate:facing", 0}, {"levitate:face", 0}});
    dsp::nixie::track(q);
    dsp::nixie::updateRedstone(r, q, true);
    ctx.check(dsp::nixie::displayed(r, q) == "00", "no power: 00");
    ctx.setBlock(at(q, 1, 0, 0), "minecraft:redstone_block");
    co_await ctx.ticks(6);
    ctx.check(dsp::nixie::displayed(r, q) == "15", "redstone block: 15, got " + dsp::nixie::displayed(r, q));
    ctx.setBlock(at(q, 1, 0, 0), "minecraft:air");
    co_await ctx.ticks(6);
    ctx.check(dsp::nixie::displayed(r, q) == "00", "power gone: 00");
});

LV_REGISTER_SELFTEST("display.link_transfer", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(2440), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& r = region(ctx);
    // source: a nixie tube showing "42"; link on top of it; target: a 2-wide board
    ctx.setBlock(p, dsp::kNixieId, {{"levitate:facing", 0}, {"levitate:face", 0}});
    dsp::nixie::track(p);
    dsp::nixie::displayCustomText(r, p, "42");
    auto boardAt = at(p, 3, 0, 0);
    placeBoards(ctx, boardAt, 2, 1, 0);
    auto link = up(p, 1);
    ctx.require(placeLink(ctx, link, "up", boardAt), "place the link");
    auto src = dsp::link::sourcePos(r, link);
    ctx.check(src && *src == p, "the link reads the block below");
    dsp::link::selectSource(link, "nixie_tube");
    ctx.check(dsp::link::update(link), "update transfers");
    auto ctrl = dsp::board::structureOf(r, boardAt)->controller;
    ctx.check(dsp::board::lineText(ctrl, 0) == "42", "board line 0 shows 42, got '" + dsp::board::lineText(ctrl, 0) + "'");
    // a nixie change notifies the link at once (Create's notifyGatherers)
    dsp::nixie::displayCustomText(r, p, "77");
    ctx.check(dsp::board::lineText(ctrl, 0) == "77", "gatherers notified");
    // redstone: a powered link sends nothing, unpowering sends at once
    ctx.setBlock(at(link, 1, 0, 0), "minecraft:redstone_block");
    co_await ctx.ticks(4);
    auto const& lb = r.getBlock(lv::game::blockPos(link));
    ctx.check(dsp::intState(lb, "levitate:powered").value_or(0) == 1, "link powered");
    dsp::nixie::displayCustomText(r, p, "13");
    ctx.check(dsp::board::lineText(ctrl, 0) == "77", "a powered link sends nothing");
    ctx.setBlock(at(link, 1, 0, 0), "minecraft:air");
    co_await ctx.ticks(4);
    ctx.check(dsp::board::lineText(ctrl, 0) == "13", "unpowered: sent immediately, got '" + dsp::board::lineText(ctrl, 0) + "'");
    // line selection
    auto d = dsp::link::read(link);
    d->line = 1;
    dsp::link::write(link, *d);
    dsp::link::update(link);
    ctx.check(dsp::board::lineText(ctrl, 1) == "13", "line 1 written");
});

LV_REGISTER_SELFTEST("display.sign_target", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(2450), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& r = region(ctx);
    ctx.setBlock(p, dsp::kNixieId, {{"levitate:facing", 0}, {"levitate:face", 0}});
    dsp::nixie::track(p);
    dsp::nixie::displayCustomText(r, p, "HELLO");
    auto sign = at(p, 2, 0, 0);
    ctx.require(ctx.setBlock(sign, "minecraft:standing_sign"), "place a sign");
    co_await ctx.ticks(1);
    auto link = up(p, 1);
    ctx.require(placeLink(ctx, link, "up", sign), "place the link");
    dsp::link::selectSource(link, "nixie_tube");
    auto d  = dsp::link::read(link);
    d->line = 1;
    dsp::link::write(link, *d);
    ctx.check(dsp::link::update(link), "update");
    auto text = dsp::signText(r, sign);
    ctx.check(text == "\nHELLO", "sign line 2 written, got '" + text + "'");
});

LV_REGISTER_SELFTEST("display.link_screen", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(2460), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& r = region(ctx);
    auto& pl = ctx.player();
    ctx.setBlock(p, dsp::kNixieId, {{"levitate:facing", 0}, {"levitate:face", 0}});
    dsp::nixie::track(p);
    auto boardAt = at(p, 2, 0, 0);
    placeBoards(ctx, boardAt, 1, 2, 0);
    auto link = up(p, 1);
    ctx.require(placeLink(ctx, link, "up", boardAt), "place the link");
    net::setTestCompanion(pl, true);
    (void)net::takeTestMessages(pl);
    dsp::link::openScreen(pl, link);
    auto msgs  = net::takeTestMessages(pl);
    bool found = false;
    for (auto const& m : msgs) {
        if (m.channel != "screen.open" || m.payload.value("type", "") != "display.link") continue;
        auto const& data = m.payload["data"];
        found            = data["sources"].size() == 1 && data["sources"][0].value("id", "") == "nixie_tube" && data["lines"].size() == 4;
    }
    ctx.check(found, "screen.open with the nixie source and 4 board lines");
    JsonT context = {{"pos", JsonT::array({link.dim, link.x, link.y, link.z})}};
    net::injectFromClient(pl, "screen.action", {{"type", "display.link"}, {"context", context}, {"action", {{"op", "source"}, {"id", "nixie_tube"}}}});
    net::injectFromClient(pl, "screen.action", {{"type", "display.link"}, {"context", context}, {"action", {{"op", "line"}, {"value", 2}}}});
    auto d = dsp::link::read(link);
    ctx.check(d && d->sourceId == "nixie_tube" && d->line == 2, "source and line set by screen actions");
    auto ctrl = dsp::board::structureOf(r, boardAt)->controller;
    ctx.check(dsp::board::lineText(ctrl, 2) == "0", "the configured link wrote line 3 ('0' redstone), got '" + dsp::board::lineText(ctrl, 2) + "'");
    net::setTestCompanion(pl, false);
});

LV_REGISTER_SELFTEST("display.clipboard_screen", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(2470), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& pl   = ctx.player();
    auto  clip = items::make(dsp::kClipboardId);
    ctx.require(clip.has_value(), "clipboard item");
    int slot = pl.getSelectedItemSlot();
    ctx.require(ctx.give(pl, *clip, slot) == slot, "clipboard in hand");
    net::setTestCompanion(pl, true);
    JsonT context = {{"slot", slot}};
    auto  act     = [&](JsonT action) {
        net::injectFromClient(pl, "screen.action", {{"type", "display.clipboard"}, {"context", context}, {"action", std::move(action)}});
    };
    act({{"op", "set"}, {"page", 0}, {"entries", JsonT::array({{{"t", "Iron"}, {"c", false}}, {{"t", "Gold"}, {"c", false}}, {{"t", ""}, {"c", false}}})}});
    auto c = dsp::clipboard::ofItem(pl.getSelectedItem());
    ctx.check(c.type == 2 && c.pages.size() == 1 && c.pages[0].size() == 2, "two entries written (blank dropped)");
    act({{"op", "toggle"}, {"page", 0}, {"entry", 0}});
    c = dsp::clipboard::ofItem(pl.getSelectedItem());
    ctx.check(!c.pages.empty() && c.pages[0][0].checked, "entry checked");
    act({{"op", "erase_checked"}, {"page", 0}});
    c = dsp::clipboard::ofItem(pl.getSelectedItem());
    ctx.check(c.pages.size() == 1 && c.pages[0].size() == 1 && c.pages[0][0].text == "Gold", "checked entry erased");
    auto last = dsp::clipboard::lastViewedEntries(c);
    ctx.check(last.size() == 1 && last[0].text == "Gold", "last viewed entries");
    // the clipboard writes its first entry onto a nixie tube
    auto& r = region(ctx);
    ctx.setBlock(p, dsp::kNixieId, {{"levitate:facing", 0}, {"levitate:face", 0}});
    dsp::nixie::track(p);
    ctx.check(dsp::nixie::use(pl, r, p, pl.getSelectedItem()), "clipboard used on a nixie");
    ctx.check(dsp::nixie::displayed(r, p) == "Go", "nixie shows 'Go'");
    net::setTestCompanion(pl, false);
    pl.setSelectedItem(ItemStack::EMPTY_ITEM());
});

LV_REGISTER_SELFTEST("display.clipboard_settings", [](Context& ctx) -> ll::coro::CoroTask<> {
    namespace vb = lv::feature::common::valuebox;
    auto p = up(ctx.testPos(2480), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& r  = region(ctx);
    auto& pl = ctx.player();
    static std::map<GlobalPos, int> values;
    values.clear();
    char const* block = "minecraft:lodestone";
    vb::registerBox(block, {
        .id = "display_test", .label = "", .labelFallback = "Test",
        .board  = [](Block const&) { return lv::core::common::plainBoard(16, 4, "x"); },
        .centre = [](Block const&, int) -> std::optional<lv::core::common::Vec> { return lv::core::common::Vec{0.5, 1, 0.5}; },
        .get    = [](GlobalPos const& gp) { return lv::core::common::ValueSettings{0, values[gp]}; },
        .set    = [](GlobalPos const& gp, lv::core::common::ValueSettings s, Player&) { values[gp] = s.value; },
    });
    auto a = p, b = at(p, 2, 0, 0);
    ctx.setBlock(a, block);
    ctx.setBlock(b, block);
    values[a] = 7;
    auto copied = dsp::clipboard::copySettings(r, a);
    ctx.check(copied.contains("ScrollValue"), "value box copied");
    ctx.check(dsp::clipboard::pasteSettings(pl, r, b, copied, true), "paste possible");
    ctx.check(values[b] == 0, "simulation changes nothing");
    dsp::clipboard::pasteSettings(pl, r, b, copied, false);
    ctx.check(values[b] == 7, "pasted value 7, got " + std::to_string(values[b]));
    ctx.check(dsp::clipboard::copySettings(r, at(p, 4, 0, 0)).is_null(), "air has nothing to copy");
    vb::unregisterBox(block, "display_test");
});

LV_REGISTER_SELFTEST("display.link_item", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto p = up(ctx.testPos(2490), 1);
    ctx.require(co_await ctx.ensureLoaded(p), "test area loaded");
    auto& r  = region(ctx);
    auto& sp = static_cast<SimulatedPlayer&>(ctx.player());
    placeBoards(ctx, at(p, 0, 1, 0), 1, 1, 0);
    ctx.setBlock(at(p, 0, 0, 2), "minecraft:stone");
    ctx.cleanupBlock(at(p, 0, 1, 2));
    auto item = items::make(dsp::kLinkId);
    ctx.require(item.has_value(), "display link item");
    ctx.require(ctx.give(sp, *item, 0) == 0, "link in slot 0");
    (*sp.mInventory).selectSlot(0, ContainerID::Inventory);
    sp.teleport(Vec3{p.x - 1.5f, float(p.y), p.z + 1.5f}, DimensionType(p.dim));
    co_await ctx.ticks(2);
    // first click: the target
    sp.simulateUseItemInSlotOnBlock(0, lv::game::blockPos(at(p, 0, 1, 0)), ScriptModuleMinecraft::ScriptFacing::South, Vec3{0.5f, 0.5f, 1.0f});
    auto sel = items::getData(sp.getInventory().getItem(0), "display_link");
    ctx.check(sel.is_object() && sel.contains("pos"), "target selected on the item");
    co_await ctx.ticks(6);
    // second click: placed on top of the stone, bound to the board
    sp.simulateLookAt(Vec3{p.x + 0.5f, p.y + 1.0f, p.z + 2.5f}, sim::LookDuration::Continuous);
    co_await ctx.ticks(2);
    bool usedOk = sp.simulateUseItemInSlotOnBlock(0, lv::game::blockPos(at(p, 0, 0, 2)), ScriptModuleMinecraft::ScriptFacing::Up, Vec3{0.5f, 1.0f, 0.5f});
    ctx.info(std::string("second use returned ") + (usedOk ? "true" : "false") + ", item " + items::idOf(sp.getInventory().getItem(0)));
    co_await ctx.ticks(2);
    auto link = at(p, 0, 1, 2);
    ctx.check(lv::game::blockIdAt(r, lv::game::blockPos(link)) == dsp::kLinkId, "link placed, found " + lv::game::blockIdAt(r, lv::game::blockPos(link)));
    auto d = dsp::link::read(link);
    ctx.check(d && d->targetOffset == std::array<int, 3>{0, 0, -2}, "bound to the board");
    auto src = dsp::link::sourcePos(r, link);
    ctx.check(src && *src == at(p, 0, 0, 2), "attached to the stone");
});
