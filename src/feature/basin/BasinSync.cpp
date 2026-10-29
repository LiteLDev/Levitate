// What the client mod needs to animate the unit's blocks (press head, mixer whisk, burner, basin
// contents): "basin.machines" {dim, list: [{p: [x, y, z], k: kind, ...}]} over the companion
// protocol (U3). Changed machines are sent at the end of the tick to companions within 96 blocks;
// every 40 ticks each companion gets every machine around it (joins, chunk loads).

#include <set>

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"

#include "feature/basin/BasinInternal.h"
#include "feature/net/Net.h"
#include "game/Blocks.h"

namespace lv::feature::basin::sync {

namespace {

constexpr float kRange = 96.f;

std::set<GlobalPos>& dirtySet() {
    static std::set<GlobalPos> s;
    return s;
}

std::optional<Json> describe(GlobalPos const& pos) {
    auto* region = regionOf(pos.dim);
    if (!region) return std::nullopt;
    auto bp = game::blockPos(pos);
    if (!loaded(*region, bp)) return std::nullopt;
    auto kind = kindOf(idAt(*region, bp));
    if (!kind) return Json{{"p", {pos.x, pos.y, pos.z}}, {"k", -1}};
    Json j{{"p", {pos.x, pos.y, pos.z}}, {"k", static_cast<int>(*kind)}};
    switch (*kind) {
    case Kind::Basin:
        if (auto* b = basinops::get(*region, pos)) j["basin"] = basinops::renderJson(*b);
        break;
    case Kind::Press:
    case Kind::Mixer:
        if (auto* s = operators::get(pos, *kind == Kind::Press)) j["op"] = operators::renderJson(pos, *s, *kind == Kind::Press);
        break;
    case Kind::Burner:
        if (auto* d = burners::get(pos)) j["goggles"] = d->goggles;
        break;
    default:
        break;
    }
    return j;
}

bool near(Player const& p, GlobalPos const& pos) {
    if (static_cast<int>(p.getDimensionId()) != pos.dim) return false;
    auto const& at = p.getPosition();
    float       dx = at.x - pos.x, dy = at.y - pos.y, dz = at.z - pos.z;
    return dx * dx + dy * dy + dz * dz <= kRange * kRange;
}

void sendTo(Player& p, int dim, std::vector<Json> const& list) {
    if (list.empty()) return;
    net::send(p, "basin.machines", {{"dim", dim}, {"list", list}});
}

} // namespace

void dirty(GlobalPos const& pos) { dirtySet().insert(pos); }

void reset() { dirtySet().clear(); }

void flush() {
    auto level = ll::service::getLevel();
    if (!level) return;
    bool full    = now() % 40 == 0;
    auto changed = std::exchange(dirtySet(), {});
    if (changed.empty() && !full) return;
    std::map<GlobalPos, Json> described;
    auto get = [&](GlobalPos const& pos) -> Json const* {
        auto it = described.find(pos);
        if (it == described.end()) {
            auto j = describe(pos);
            if (!j) return nullptr;
            it = described.emplace(pos, std::move(*j)).first;
        }
        return &it->second;
    };
    level->forEachPlayer([&](Player& p) {
        if (!net::isCompanion(p)) return true;
        int               dim = static_cast<int>(p.getDimensionId());
        std::vector<Json> list;
        if (full) {
            for (auto const& [pos, kind] : tracked())
                if (near(p, pos))
                    if (auto const* j = get(pos)) list.push_back(*j);
        } else {
            for (auto const& pos : changed)
                if (near(p, pos))
                    if (auto const* j = get(pos)) list.push_back(*j);
        }
        sendTo(p, dim, list);
        return true;
    });
}

} // namespace lv::feature::basin::sync
