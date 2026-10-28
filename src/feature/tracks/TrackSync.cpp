// Tracks unit: curve sync to clients.
//
// Bedrock blocks can't show curves, so the client mod draws them (src/client/tracks). The server
// sends the curves as script messages (id "levitate:tracks", JSON value):
//   {"op":"reset"}                                  on join, before the full list
//   {"op":"add","dim":d,"curves":[<curve json>...]} new curves (primary orientation)
//   {"op":"remove","dim":d,"curves":[[a],[b]]...}   curves removed, by their end blocks
// Vanilla clients ignore script messages; they see the straight tracks and the fake track blocks
// along the curve (SPEC §8).

#include "mc/network/packet/ScriptMessagePacket.h"
#include "mc/world/actor/player/Player.h"

#include "feature/tracks/TracksInternal.h"

namespace lv::feature::tracks {

namespace {
constexpr char const* kChannel  = "levitate:tracks";
constexpr size_t      kBatch    = 24; ///< curves per message (keeps packets small)

void send(nlohmann::json const& msg, Player* to) {
    ScriptMessagePacket pkt(ScriptMessagePacketPayload(kChannel, msg.dump()));
    if (to) pkt.sendTo(*to);
    else pkt.sendToClients();
}

nlohmann::json keyOf(ct::Curve const& c) {
    auto a = c.block(0), b = c.block(1);
    return nlohmann::json::array({nlohmann::json::array({a.x, a.y, a.z}), nlohmann::json::array({b.x, b.y, b.z})});
}
} // namespace

void syncCurveAdded(int dim, ct::Curve const& curve) {
    send({{"op", "add"}, {"dim", dim}, {"curves", nlohmann::json::array({curve.toJson()})}}, nullptr);
}

void syncCurveRemoved(int dim, ct::Curve const& curve) {
    send({{"op", "remove"}, {"dim", dim}, {"curves", nlohmann::json::array({keyOf(curve)})}}, nullptr);
}

void syncAllTo(Player& player) {
    send({{"op", "reset"}}, &player);
    std::map<int, nlohmann::json> batches;
    auto flush = [&](int dim) {
        auto& b = batches[dim];
        if (b.empty()) return;
        send({{"op", "add"}, {"dim", dim}, {"curves", b}}, &player);
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
