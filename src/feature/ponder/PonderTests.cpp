// Selftests of the server side of Ponder (`ponder.*`). The client screens are covered by the
// client e2e scenario `ponder` (tools/e2e/client/scenarios/ponder.ps1).

#include <filesystem>
#include <fstream>

#include "mc/world/actor/player/Player.h"

#include "core/ponder/Scene.h"
#include "feature/net/Net.h"
#include "feature/ponder/Ponder.h"
#include "mod/Main.h"
#include "selftest/SelfTest.h"

namespace lv::feature::ponder {

using selftest::Context;

LV_REGISTER_SELFTEST("ponder.data", [](Context& ctx) {
    auto const& lib = index();
    ctx.check(lib.allComponents().size() >= 150, "the index lists Create's ponder entries (" + std::to_string(lib.allComponents().size()) + ")");
    ctx.check(lib.tags().size() >= 18, "the index has Create's tags");
    auto dir   = lv::Mod::getInstance().getSelf().getModDir() / "ponder" / "scenes";
    int  files = 0, bad = 0;
    std::error_code ec;
    for (auto const& e : std::filesystem::directory_iterator(dir, ec)) {
        std::ifstream in(e.path());
        auto          r = core::ponder::parseScene(nlohmann::json::parse(in, nullptr, false));
        ++files;
        if (!r.scene) {
            ++bad;
            ctx.info(e.path().filename().string() + ": " + (r.errors.empty() ? "invalid" : r.errors.front()));
        }
    }
    ctx.check(files >= 167 && bad == 0, "every shipped scene parses (" + std::to_string(files) + " files, " + std::to_string(bad) + " invalid)");
});

LV_REGISTER_SELFTEST("ponder.resolve", [](Context& ctx) {
    ctx.check(resolve("") == std::string{}, "no id: the index");
    ctx.check(resolve("shaft") == std::string("levitate:shaft"), "short ids get the levitate namespace");
    ctx.check(resolve("levitate:large_cogwheel") == std::string("levitate:large_cogwheel"), "full ids");
    ctx.check(resolve("kinetic_relays") == std::string("kinetic_relays"), "tag ids");
    ctx.check(!resolve("minecraft:dirt").has_value(), "items without scenes are refused");
});

LV_REGISTER_SELFTEST("ponder.command", [](Context& ctx) -> ll::coro::CoroTask<> {
    auto& p = ctx.player();
    co_await ctx.ticks(1);
    // a vanilla client: refused, nothing sent
    auto r = request(p, "shaft");
    ctx.check(!r.ok, "players without the client mod are refused");
    // a companion: the client is told to open the entry
    net::setTestCompanion(p, true);
    r         = request(p, "shaft");
    auto msgs = net::takeTestMessages(p);
    ctx.check(r.ok && msgs.size() == 1 && msgs[0].channel == "ponder.open" && msgs[0].payload.value("id", "") == "levitate:shaft",
              "ponder.open carries the entry");
    r    = request(p, "");
    msgs = net::takeTestMessages(p);
    ctx.check(r.ok && msgs.size() == 1 && msgs[0].payload.value("id", "x").empty(), "no id opens the index");
    r    = request(p, "no_such_thing");
    msgs = net::takeTestMessages(p);
    ctx.check(!r.ok && msgs.empty(), "unknown ids are refused");
    net::setTestCompanion(p, false);
});

} // namespace lv::feature::ponder
