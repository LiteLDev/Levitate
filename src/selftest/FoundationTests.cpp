// Selftests of the foundation: world storage, translations, configs, blocks and simulated players.

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Player.h"

#include "game/Blocks.h"
#include "game/WorldDb.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/ModConfig.h"
#include "selftest/SelfTest.h"

namespace {
using lv::selftest::Context;
}

LV_REGISTER_SELFTEST("foundation.world_db", [](Context& ctx) {
    namespace db = lv::game::db;
    ctx.require(db::available(), "a world is loaded");
    std::string const key = "lv:foundation:probe";
    db::save(key, {{"value", 42}});
    auto back = db::load(key);
    ctx.check(back && back->value("value", 0) == 42, "saved JSON reads back");
    int seen = 0;
    db::forEachKey("lv:foundation:", [&seen](std::string_view suffix) {
        if (suffix == "probe") ++seen;
    });
    ctx.check(seen == 1, "forEachKey finds the key by prefix");
    db::erase(key);
    ctx.check(!db::load(key), "erased key is gone");
});

LV_REGISTER_SELFTEST("foundation.i18n", [](Context& ctx) {
    ctx.check(lv::tr("lv.test.missing", "Fallback {}", 1) == "Fallback 1", "missing keys use the fallback");
    ctx.check(lv::translate("lv.mod.name", "?", "en_US") == "Levitate", "en_US lang file is loaded");
    ctx.check(lv::translate("lv.mod.name", "?", "zh_CN") != "?", "zh_CN lang file is loaded");
    ctx.check(!lv::config::configDir().empty(), "config directory is set");
    ctx.check(lv::config::get<lv::ModConfig>().version >= 1, "mod config is loaded");
});

LV_REGISTER_SELFTEST("foundation.blocks", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto pos = ctx.testPos(10);
    ctx.require(co_await ctx.ensureLoaded(pos), "test area loaded");
    ctx.require(ctx.setBlock(pos, "minecraft:gold_block"), "set a vanilla block");
    co_await ctx.ticks(1);
    auto& region = ctx.player().getDimensionBlockSource();
    ctx.check(lv::game::blockIdAt(region, lv::game::blockPos(pos)) == "minecraft:gold_block", "block reads back");
    ctx.check(lv::game::blockWith("levitate:no_such_block") == nullptr, "unknown block ids give nullptr");
});

LV_REGISTER_SELFTEST("foundation.players", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& a = ctx.player();
    auto& b = ctx.spawnPlayer();
    co_await ctx.ticks(2);
    ctx.check(&a != static_cast<Player*>(&b), "two distinct simulated players");
    ctx.check(!a.getRealName().empty(), "the simulated player has a name");
});
