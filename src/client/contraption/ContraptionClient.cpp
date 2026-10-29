// Client mod: which display entity shows which contraption block, and actor visuals (see
// ContraptionClient.h; the server side is in feature/contraption/Engine.cpp).

#ifdef LV_CLIENT

#include "client/contraption/ContraptionClient.h"

#include <map>
#include <tuple>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/Level.h"

#include "client/Client.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::contraption {

namespace {

using Json = nlohmann::json;
namespace cc = core::contraption;

struct Block {
    std::int64_t display = 0;
    cc::Pos      local;
    std::string  id;
};
struct Known {
    int                                        dim = 0, axis = 1, quarter = 0;
    std::vector<Block>                         blocks;
    std::map<std::tuple<int, int, int>, Json>  visuals;
};

std::map<std::uint64_t, Known>& known() {
    static std::map<std::uint64_t, Known> k;
    return k;
}

cc::Pos posOf(Json const& j, std::size_t at) { return {j[at].get<int>(), j[at + 1].get<int>(), j[at + 2].get<int>()}; }

void onBlocks(Json const& msg) {
    auto  id = msg.value("id", std::uint64_t{0});
    auto& k  = known()[id];
    k.dim     = msg.value("dim", 0);
    k.axis    = msg.value("axis", 1);
    k.quarter = msg.value("quarter", 0);
    k.blocks.clear();
    for (auto const& b : msg.value("blocks", Json::array())) {
        if (!b.is_array() || b.size() != 5) continue;
        k.blocks.push_back({b[0].get<std::int64_t>(), posOf(b, 1), b[4].get<std::string>()});
    }
    k.visuals.clear();
    for (auto const& v : msg.value("visuals", Json::array())) {
        if (!v.is_array() || v.size() != 4) continue;
        k.visuals[{v[0].get<int>(), v[1].get<int>(), v[2].get<int>()}] = v[3];
    }
}

void onVisual(Json const& msg) {
    auto it = known().find(msg.value("id", std::uint64_t{0}));
    auto l  = msg.value("local", Json::array());
    if (it == known().end() || !l.is_array() || l.size() != 3) return;
    auto key = std::tuple{l[0].get<int>(), l[1].get<int>(), l[2].get<int>()};
    auto v   = msg.value("visual", Json());
    if (v.is_null()) it->second.visuals.erase(key);
    else it->second.visuals[key] = std::move(v);
}

} // namespace

void forEachBlock(std::string const& blockId, float partialTicks, std::function<void(BlockView const&)> const& fn) {
    auto* ci     = currentClient();
    auto* player = currentPlayer();
    auto* level  = ci ? ci->getLevel() : nullptr;
    if (!level || !player || known().empty()) return;
    int dim = static_cast<int>(player->getDimensionId());
    for (auto const& [id, k] : known()) {
        if (k.dim != dim) continue;
        for (auto const& b : k.blocks) {
            if (!blockId.empty() && b.id != blockId) continue;
            ActorUniqueID uid;
            uid.rawID = b.display;
            auto* actor = level->fetchEntity(uid, false);
            if (!actor) continue;
            auto      feet = actor->getInterpolatedPosition(partialTicks);
            BlockView v;
            v.contraption = id;
            v.local       = b.local;
            v.blockId     = b.id;
            v.center      = {feet.x, feet.y + 0.5, feet.z};
            v.axis        = k.axis;
            // the display shows the nearest quarter turn; its yaw carries the rest (Internal.h)
            v.angle = k.quarter * 90.0 + actor->getRotation().y;
            auto vis = k.visuals.find({b.local.x, b.local.y, b.local.z});
            v.visual = vis == k.visuals.end() ? nullptr : &vis->second;
            fn(v);
        }
    }
}

class ContraptionClientModule {
public:
    void install() {
        namespace net = feature::net;
        net::client::on("contraption.blocks", [](Json const& m) { onBlocks(m); });
        net::client::on("contraption.visual", [](Json const& m) { onVisual(m); });
        net::client::on("contraption.remove", [](Json const& m) { known().erase(m.value("id", std::uint64_t{0})); });
        net::client::on("contraption.reset", [](Json const&) { known().clear(); });
        mExit = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) { known().clear(); });
    }
    void uninstall() { ll::event::EventBus::getInstance().removeListener(mExit); }

private:
    ll::event::ListenerPtr mExit;
};

} // namespace lv::client::contraption

LV_REGISTER_MODULE(lv::client::contraption::ContraptionClientModule);

#endif // LV_CLIENT
