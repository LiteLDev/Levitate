// Create's config screen (U42, SPEC §5): `/create config [section]` lists the config sections
// (`config/<section>.json`) and edits their entries in game. Companions get the native screen
// (client/screen/ConfigScreen.cpp: Create's config GUI, a scrolled list of the entries with
// toggles for booleans and text boxes for numbers and text, reset to default), vanilla players
// a form. Everybody may look; changing a value needs operator permission (Create: the server
// config of a server can only be changed by operators). A change writes the section's file and
// reloads every config as `/create reload` does.

#include <cmath>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/actor/player/Player.h"

#include "feature/common/Screens.h"
#include "feature/kinetics/KineticsConfig.h"
#include "feature/net/Net.h"
#include "mod/Commands.h"
#include "mod/Config.h"
#include "mod/I18n.h"
#include "mod/Log.h"
#include "mod/Module.h"
#include "selftest/SelfTest.h"

namespace lv::feature::common {

namespace {

using Json    = nlohmann::json;
using OJson   = nlohmann::ordered_json;
constexpr char kType[] = "common.config";

config::ConfigEntry const* entryOf(std::string const& section) {
    for (auto const& e : config::ConfigRegistry::entries())
        if (e.section == section) return &e;
    return nullptr;
}

bool mayEdit(Player const& player) { return player.getCommandPermissionLevel() >= CommandPermissionLevel::GameDirectors; }

/// The editable leaves of a section: "path.to.key" -> value (booleans, numbers, text; `version`
/// is not editable), in file order.
void flatten(OJson const& j, std::string const& prefix, std::vector<std::pair<std::string, OJson>>& out) {
    for (auto it = j.begin(); it != j.end(); ++it) {
        std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (prefix.empty() && it.key() == "version") continue;
        if (it->is_object()) flatten(*it, path, out);
        else if (it->is_boolean() || it->is_number() || it->is_string()) out.emplace_back(path, *it);
    }
}

OJson const* at(OJson const& j, std::string const& path) {
    OJson const* cur = &j;
    std::size_t  start = 0;
    while (true) {
        auto        dot = path.find('.', start);
        std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!cur->is_object()) return nullptr;
        auto it = cur->find(key);
        if (it == cur->end()) return nullptr;
        cur = &*it;
        if (dot == std::string::npos) return cur;
        start = dot + 1;
    }
}

/// `{"a": {"b": value}}` for path "a.b".
OJson patchFor(std::string const& path, OJson const& value) {
    OJson       patch = value;
    std::size_t end   = path.size();
    while (true) {
        auto        dot = path.rfind('.', end - 1);
        std::size_t from = dot == std::string::npos ? 0 : dot + 1;
        OJson       wrap = OJson::object();
        wrap[path.substr(from, end - from)] = std::move(patch);
        patch = std::move(wrap);
        if (dot == std::string::npos) return patch;
        end = dot;
    }
}

/// The value typed in for an entry of type `like` (numbers from text), nullopt if it doesn't fit.
std::optional<OJson> coerce(OJson const& like, Json const& input) {
    if (like.is_boolean()) {
        if (input.is_boolean()) return OJson(input.get<bool>());
        return std::nullopt;
    }
    if (like.is_string()) return input.is_string() ? std::optional<OJson>(OJson(input.get<std::string>())) : std::nullopt;
    double v = 0;
    if (input.is_number()) v = input.get<double>();
    else if (input.is_string()) {
        try {
            std::size_t used = 0;
            auto        s    = input.get<std::string>();
            v                = std::stod(s, &used);
            if (used != s.size()) return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    } else return std::nullopt;
    if (!std::isfinite(v)) return std::nullopt;
    if (like.is_number_integer()) {
        if (v != std::floor(v)) return std::nullopt;
        return OJson(static_cast<std::int64_t>(v));
    }
    return OJson(v);
}

/// Sets one entry (operators only); writes the file and reloads every config.
bool setEntry(Player& player, std::string const& section, std::string const& path, Json const& input, bool reset) {
    auto const* e = entryOf(section);
    if (!e || !e->values || !e->write) return false;
    if (!mayEdit(player)) {
        player.sendMessage("§c" + trFor(player, "lv.common.config.need_op", "Only operators can change the configuration."));
        return false;
    }
    auto        current = e->values();
    auto const* old     = at(current, path);
    if (!old || old->is_object() || path == "version") return false;
    std::optional<OJson> value;
    if (reset) {
        auto defaults = e->defaults();
        if (auto const* d = at(defaults, path)) value = *d;
    } else {
        value = coerce(*old, input);
    }
    if (!value) {
        player.sendMessage("§c" + trFor(player, "lv.common.config.invalid", "Invalid value for {}", path));
        return false;
    }
    if (*value == *old) return true;
    if (!e->write(patchFor(path, *value), config::configDir() / (section + ".json"))) return false;
    config::loadAll();
    log().info("config {}.{} set to {} by {}", section, path, value->dump(), player.getRealName());
    return true;
}

std::optional<Json> screenData(Player& player, Json const& ctx) {
    Json out{{"admin", mayEdit(player)}};
    auto section = ctx.value("section", std::string{});
    if (section.empty()) {
        Json names = Json::array();
        for (auto const& e : config::ConfigRegistry::entries()) names.push_back(e.section);
        out["sections"] = names;
        return out;
    }
    auto const* e = entryOf(section);
    if (!e || !e->values) return std::nullopt;
    std::vector<std::pair<std::string, OJson>> cur;
    flatten(e->values(), "", cur);
    auto defaults = e->defaults();
    Json entries  = Json::array();
    for (auto const& [path, v] : cur) {
        auto const* d = at(defaults, path);
        entries.push_back({{"path", path}, {"value", Json::parse(v.dump())}, {"default", d ? Json::parse(d->dump()) : Json()}});
    }
    out["section"] = section;
    out["entries"] = entries;
    return out;
}

void sectionForm(Player& player, std::string const& section);

void rootForm(Player& player) {
    ll::form::SimpleForm form;
    form.setTitle(trFor(player, "lv.common.config.title", "Levitate Configuration"));
    form.setContent(trFor(player, "lv.common.config.pick", "Choose a section"));
    for (auto const& e : config::ConfigRegistry::entries()) {
        auto section = e.section;
        form.appendButton(section, [section](Player& p) { sectionForm(p, section); });
    }
    form.sendTo(player);
}

void sectionForm(Player& player, std::string const& section) {
    auto const* e = entryOf(section);
    if (!e || !e->values) return;
    std::vector<std::pair<std::string, OJson>> cur;
    flatten(e->values(), "", cur);
    ll::form::CustomForm form(section);
    if (!mayEdit(player)) form.appendLabel("§c" + trFor(player, "lv.common.config.need_op", "Only operators can change the configuration."));
    for (std::size_t i = 0; i < cur.size(); ++i) {
        auto const& [path, v] = cur[i];
        auto name = "e" + std::to_string(i);
        if (v.is_boolean()) form.appendToggle(name, path, v.get<bool>());
        else form.appendInput(name, path, "", v.is_string() ? v.get<std::string>() : v.dump());
    }
    form.sendTo(player, [section, cur](Player& p, ll::form::CustomFormResult const& r, ll::form::FormCancelReason) {
        if (!r) return;
        for (std::size_t i = 0; i < cur.size(); ++i) {
            auto it = r->find("e" + std::to_string(i));
            if (it == r->end()) continue;
            auto const& [path, v] = cur[i];
            Json input;
            if (auto const* b = std::get_if<uint64>(&it->second)) input = *b != 0;
            else if (auto const* s = std::get_if<std::string>(&it->second)) input = *s;
            else continue;
            // unchanged entries are skipped (a vanilla form sends every field back)
            if (v.is_boolean() ? input == Json(v.get<bool>()) : input == Json(v.is_string() ? v.get<std::string>() : v.dump())) continue;
            setEntry(p, section, path, input, false);
        }
    });
}

} // namespace

class ConfigScreenModule {
public:
    void install() {
        screens::registerType({
            .id     = kType,
            .data   = screenData,
            .action =
                [](Player& player, Json const& ctx, Json const& action) {
                    auto op = action.value("op", std::string{});
                    if (op == "open") {
                        screens::open(player, kType, {{"section", action.value("section", std::string{})}});
                        return;
                    }
                    auto section = ctx.value("section", std::string{});
                    if (op == "set" || op == "reset") setEntry(player, section, action.value("path", std::string{}), action.value("value", Json()), op == "reset");
                },
            .form =
                [](Player& player, Json const& ctx) {
                    auto section = ctx.value("section", std::string{});
                    if (section.empty()) rootForm(player);
                    else sectionForm(player, section);
                },
        });
    }
    void uninstall() {}
};

struct ConfigParams {
    std::string section;
};

LV_REGISTER_SUBCOMMAND([](ll::command::CommandHandle& cmd) {
    auto run = [](CommandOrigin const& origin, CommandOutput& output, std::string const& section) {
        auto* player = commands::playerOf(origin);
        if (!player) return output.error(tr("lv.common.config.need_player", "Run this as a player."));
        if (!section.empty() && !entryOf(section)) return output.error(tr("lv.common.config.unknown", "Unknown config section {}", section));
        Json ctx = Json::object();
        if (!section.empty()) ctx["section"] = section;
        if (!screens::open(*player, kType, ctx)) return output.error(tr("lv.common.config.unknown", "Unknown config section {}", section));
        log().info("config screen {} opened for {}", section.empty() ? "(sections)" : section, player->getRealName());
    };
    cmd.overload().text("config").execute([run](CommandOrigin const& origin, CommandOutput& output) { run(origin, output, ""); });
    cmd.overload<ConfigParams>().text("config").required("section").execute(
        [run](CommandOrigin const& origin, CommandOutput& output, ConfigParams const& p) { run(origin, output, p.section); }
    );
});

LV_REGISTER_SELFTEST("common.config_screen", [](selftest::Context& ctx) -> ll::coro::CoroTask<> {
    auto& p   = ctx.player();
    auto  was = p.getCommandPermissionLevel();
    net::setTestCompanion(p, true);
    (void)net::takeTestMessages(p);
    ctx.require(screens::open(p, kType, Json::object()), "the section list opens");
    auto msgs = net::takeTestMessages(p);
    bool listed = false;
    for (auto const& m : msgs)
        if (m.channel == "screen.open")
            for (auto const& s : m.payload["data"].value("sections", Json::array())) listed |= s == "kinetics";
    ctx.check(listed, "the sections are listed");
    auto send = [&](Json const& action) {
        net::injectFromClient(p, "screen.action", {{"type", kType}, {"context", {{"section", "kinetics"}}}, {"action", action}});
    };
    auto const& cfg    = config::get<kinetics::KineticsConfig>();
    float       before = cfg.mediumSpeed;
    p.setCommandPermissions(CommandPermissionLevel::Any);
    send({{"op", "set"}, {"path", "mediumSpeed"}, {"value", "42"}});
    ctx.check(config::get<kinetics::KineticsConfig>().mediumSpeed == before, "non-operators can't change values");
    p.setCommandPermissions(CommandPermissionLevel::GameDirectors);
    send({{"op", "set"}, {"path", "mediumSpeed"}, {"value", "42"}});
    ctx.check(config::get<kinetics::KineticsConfig>().mediumSpeed == 42.f, "an operator sets a number");
    send({{"op", "set"}, {"path", "mediumSpeed"}, {"value", "fast"}});
    ctx.check(config::get<kinetics::KineticsConfig>().mediumSpeed == 42.f, "text is refused for a number");
    send({{"op", "set"}, {"path", "impact.mechanical_press"}, {"value", "9"}});
    ctx.check(config::get<kinetics::KineticsConfig>().impact.at("mechanical_press") == 9.0, "nested entries are set");
    send({{"op", "reset"}, {"path", "impact.mechanical_press"}});
    send({{"op", "reset"}, {"path", "mediumSpeed"}});
    ctx.check(config::get<kinetics::KineticsConfig>().impact.at("mechanical_press") == 8.0, "reset restores the default");
    ctx.check(config::get<kinetics::KineticsConfig>().mediumSpeed == kinetics::KineticsConfig{}.mediumSpeed, "reset restores the default number");
    if (before != kinetics::KineticsConfig{}.mediumSpeed) send({{"op", "set"}, {"path", "mediumSpeed"}, {"value", std::to_string(before)}});
    p.setCommandPermissions(was);
    net::setTestCompanion(p, false);
    co_return;
});

} // namespace lv::feature::common

LV_REGISTER_MODULE(lv::feature::common::ConfigScreenModule);
