// Tracks unit: curve sync to clients over U3's companion protocol (src/feature/net/Net.h).
//
// Bedrock blocks can't show curves, so the client mod draws them (src/client/tracks). Companions
// (players whose client mod completed the handshake) get, on channel "tracks.curves":
//   {"op":"reset"}                                  after the handshake, before the full list
//   {"op":"add","dim":d,"curves":[<curve json>...]} new curves (primary orientation)
//   {"op":"remove","dim":d,"curves":[[a],[b]]...}   curves removed, by their end blocks
// The full list holds the curves of every dimension, so a dimension change needs no resync.
// Vanilla clients get nothing; they see the straight tracks and the fake track blocks along the
// curve (SPEC §8).

#include "ll/api/service/Bedrock.h"

#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "feature/net/Net.h"
#include "feature/tracks/TracksInternal.h"

namespace lv::feature::tracks {

namespace {
constexpr size_t kBatch = 24; ///< curves per message (keeps packets small)

void sendToCompanions(nlohmann::json const& msg) {
    auto level = ll::service::getLevel();
    if (!level) return;
    level->forEachPlayer([&](Player& p) {
        net::send(p, kSyncChannel, msg); // no-op for vanilla clients
        return true;
    });
}

nlohmann::json keyOf(ct::Curve const& c) {
    auto a = c.block(0), b = c.block(1);
    return nlohmann::json::array({nlohmann::json::array({a.x, a.y, a.z}), nlohmann::json::array({b.x, b.y, b.z})});
}
} // namespace

void syncCurveAdded(int dim, ct::Curve const& curve) {
    sendToCompanions({{"op", "add"}, {"dim", dim}, {"curves", nlohmann::json::array({curve.toJson()})}});
}

void syncCurveRemoved(int dim, ct::Curve const& curve) {
    sendToCompanions({{"op", "remove"}, {"dim", dim}, {"curves", nlohmann::json::array({keyOf(curve)})}});
}

void syncSelection(Player& player) {
    if (!net::isCompanion(player)) return;
    auto const& sels = state().selections;
    auto        it   = sels.find(player.getUuid().asString());
    if (it == sels.end()) {
        net::send(player, kSelectionChannel, nlohmann::json::object());
        return;
    }
    net::send(player, kSelectionChannel, {{"dim", it->second.dim}, {"end", it->second.end.toJson()}, {"maxLength", config().maxTrackPlacementLength}});
}

bool maximiseTurn(Player const& player) {
    if (net::isCompanion(player)) return state().ctrlHeld.contains(player.getUuid().asString());
    return player.isSprinting(); // vanilla clients: no key state, sprinting stands for Ctrl
}

void syncAllTo(Player& player) {
    syncSelection(player);
    if (!net::isCompanion(player)) return;
    net::send(player, kSyncChannel, {{"op", "reset"}});
    std::map<int, nlohmann::json> batches;
    auto flush = [&](int dim) {
        auto& b = batches[dim];
        if (b.empty()) return;
        net::send(player, kSyncChannel, {{"op", "add"}, {"dim", dim}, {"curves", b}});
        b = nlohmann::json::array();
    };
    for (auto const& [dim, c] : state().curves.all()) {
        auto& b = batches[dim];
        if (b.is_null()) b = nlohmann::json::array();
        b.push_back(c->toJson());
        if (b.size() >= kBatch) flush(dim);
    }
    for (auto& [dim, b] : batches) flush(dim);
}

} // namespace lv::feature::tracks
