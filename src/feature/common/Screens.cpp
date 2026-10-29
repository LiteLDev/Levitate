#include "feature/common/Screens.h"

#include <unordered_map>

#include "mc/world/actor/player/Player.h"

#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::feature::common::screens {

namespace {
std::unordered_map<std::string, ScreenType>& types() {
    static std::unordered_map<std::string, ScreenType> m;
    return m;
}

/// The native server screen each companion has open (type, context): `screen.closed` is checked
/// against it.
std::unordered_map<std::int64_t, std::pair<std::string, Json>>& openScreens() {
    static std::unordered_map<std::int64_t, std::pair<std::string, Json>> m;
    return m;
}

ScreenType const* typeOf(std::string const& id) {
    auto it = types().find(id);
    return it == types().end() ? nullptr : &it->second;
}

void sendData(Player& player, ScreenType const& t, Json const& context, char const* channel) {
    auto data = t.data ? t.data(player, context) : std::optional<Json>(Json::object());
    if (!data) {
        net::send(player, "screen.close", {{"type", t.id}});
        return;
    }
    net::send(player, channel, {{"type", t.id}, {"context", context}, {"data", *data}});
}
} // namespace

void registerType(ScreenType type) {
    auto id      = type.id;
    types()[id] = std::move(type);
}

bool open(Player& player, std::string const& type, Json const& context) {
    auto const* t = typeOf(type);
    if (!t) {
        log().error("screen type {} is not registered", type);
        return false;
    }
    if (!net::isCompanion(player)) return openForm(player, type, context);
    auto data = t->data ? t->data(player, context) : std::optional<Json>(Json::object());
    if (!data) return false;
    net::send(player, "screen.open", {{"type", type}, {"context", context}, {"data", *data}});
    openScreens()[player.getOrCreateUniqueID().rawID] = {type, context};
    log().info("screen {} opened for {} (native)", type, player.getRealName());
    return true;
}

bool openForm(Player& player, std::string const& type, Json const& context) {
    auto const* t = typeOf(type);
    if (!t || !t->form) return false;
    if (t->data && !t->data(player, context)) return false;
    t->form(player, context);
    log().info("screen {} opened for {} (form)", type, player.getRealName());
    return true;
}

void refresh(Player& player, std::string const& type, Json const& context) {
    if (auto const* t = typeOf(type); t && net::isCompanion(player)) sendData(player, *t, context, "screen.update");
}

void close(Player& player, std::string const& type) {
    auto it = openScreens().find(player.getOrCreateUniqueID().rawID);
    if (it != openScreens().end() && it->second.first == type) openScreens().erase(it);
    net::send(player, "screen.close", {{"type", type}});
}

class ScreensModule {
public:
    void install() {
        net::on("screen.action", [](Player& player, Json const& msg) {
            auto const* t = typeOf(msg.value("type", std::string{}));
            if (!t) return;
            auto context = msg.value("context", Json::object());
            if (t->data && !t->data(player, context)) {
                close(player, t->id); // e.g. the filter left the hand
                return;
            }
            if (t->action) t->action(player, context, msg.value("action", Json::object()));
            sendData(player, *t, context, "screen.update");
        });
        net::on("screen.closed", [](Player& player, Json const& msg) {
            // only the screen the server opened for this player, with its own context
            auto it = openScreens().find(player.getOrCreateUniqueID().rawID);
            if (it == openScreens().end() || it->second.first != msg.value("type", std::string{})) return;
            auto [type, context] = std::move(it->second);
            openScreens().erase(it);
            auto const* t = typeOf(type);
            if (!t) return;
            log().info("screen {} closed by {}", t->id, player.getRealName());
            if (t->closed) t->closed(player, context);
        });
    }
    void uninstall() {
        net::off("screen.action");
        net::off("screen.closed");
    }
};

} // namespace lv::feature::common::screens

LV_REGISTER_MODULE_ORDERED(lv::feature::common::screens::ScreensModule, lv::ModuleOrder::Services);
