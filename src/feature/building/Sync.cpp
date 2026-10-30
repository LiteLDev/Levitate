// Building unit: what the client mod needs to draw copycats and brackets (client/building).
//
// Channel "building.query" (client -> server): {"chunks": [[cx, cz], ...]} for chunks of the
// player's dimension where the client meshed a copycat or a bracketable shaft / cogwheel.
// Channel "building.visuals" (server -> client): {"dim", "chunks"?: [[cx, cz], ...], "entries":
// [{x, y, z, copycat?: <material SNBT>, bracket?: {id, facing, axis, cog}}]}. A reply to a query
// lists the queried chunks (the client replaces what it knew there); a change at one position is
// pushed to every companion player of the dimension (an entry without copycat / bracket = gone).

#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "ll/api/service/Bedrock.h"

#include "feature/building/Building.h"
#include "feature/common/BlockData.h"
#include "feature/net/Net.h"
#include "mod/Log.h"

namespace lv::feature::building {

namespace bd = common::blockdata;

namespace {

nlohmann::json entryOf(GlobalPos const& pos, nlohmann::json const* unit) {
    nlohmann::json e{{"x", pos.x}, {"y", pos.y}, {"z", pos.z}};
    if (!unit) return e;
    if (unit->contains("state") && (*unit)["state"].is_string()) e["copycat"] = (*unit)["state"];
    else if (unit->contains("material") && (*unit)["material"].is_string()) e["copycat"] = "{name:\"" + (*unit)["material"].get<std::string>() + "\"}";
    if (unit->contains("bracket") && (*unit)["bracket"].is_object()) e["bracket"] = (*unit)["bracket"];
    return e;
}

void sendToDimension(int dim, nlohmann::json const& msg) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        if (net::isCompanion(p) && static_cast<int>(p.getDimensionId()) == dim) net::send(p, kVisualsChannel, msg);
        return true;
    });
}

void onQuery(Player& player, nlohmann::json const& msg) {
    auto chunks = msg.value("chunks", nlohmann::json::array());
    if (!chunks.is_array()) return;
    int            dim = static_cast<int>(player.getDimensionId());
    nlohmann::json entries = nlohmann::json::array();
    nlohmann::json done    = nlohmann::json::array();
    int            n       = 0;
    for (auto const& c : chunks) {
        if (!c.is_array() || c.size() != 2 || ++n > 256) continue;
        int cx = c[0].get<int>(), cz = c[1].get<int>();
        bd::forEachInChunk(dim, cx, cz, [&](GlobalPos const& pos, bd::Json const& doc) {
            if (!doc.contains(kDataKey)) return;
            auto const& u = doc[kDataKey];
            if (!u.contains("material") && !u.contains("bracket")) return;
            entries.push_back(entryOf(pos, &u));
        });
        done.push_back({cx, cz});
    }
    net::send(player, kVisualsChannel, {{"dim", dim}, {"chunks", done}, {"entries", entries}});
}

} // namespace

nlohmann::json visualOf(GlobalPos const& pos) {
    auto const* d = bd::get(pos);
    return entryOf(pos, d && d->contains(kDataKey) ? &(*d)[kDataKey] : nullptr);
}

void pushVisual(GlobalPos const& pos) { sendToDimension(pos.dim, {{"dim", pos.dim}, {"entries", nlohmann::json::array({visualOf(pos)})}}); }

void pushRemoved(GlobalPos const& pos) {
    sendToDimension(pos.dim, {{"dim", pos.dim}, {"entries", nlohmann::json::array({entryOf(pos, nullptr)})}});
}

void installSync() {
    net::on(kQueryChannel, onQuery);
    static bool registered = false;
    if (registered) return;
    registered = true;
    // contraptions take the data without erase listeners and put it back elsewhere
    bd::onWriteBack([](GlobalPos const* pos) {
        if (!pos) return;
        auto const* d = bd::get(*pos);
        if (d && d->contains(kDataKey) && ((*d)[kDataKey].contains("material") || (*d)[kDataKey].contains("bracket"))) pushRemoved(*pos);
    });
    bd::onPut([](GlobalPos const& pos, bd::Json const& doc) {
        if (doc.contains(kDataKey) && (doc[kDataKey].contains("material") || doc[kDataKey].contains("bracket"))) pushVisual(pos);
    });
}
void uninstallSync() { net::off(kQueryChannel); }

} // namespace lv::feature::building
