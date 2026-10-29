// Animation state for the client mod (companion protocol, U3): deployers send their phase, timer
// and hand pose when the phase changes; arms send the angles they move from / to. Only nearby
// companions get them; vanilla clients see the static models.

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include "feature/assembly/AssemblyInternal.h"
#include "feature/net/Net.h"

namespace lv::feature::assembly::sync {

namespace {
constexpr double kRange = 96.0;

void broadcast(GlobalPos const& pos, std::string_view channel, Json payload) {
    auto level = ll::service::getLevel();
    if (!level) return;
    payload["pos"]  = detail::posJson(pos);
    payload["tick"] = detail::currentTick();
    level->forEachPlayer([&](Player& p) {
        if (!net::isCompanion(p) || static_cast<int>(p.getDimensionId()) != pos.dim) return true;
        auto const& at = p.getPosition();
        double      dx = at.x - pos.x, dy = at.y - pos.y, dz = at.z - pos.z;
        if (dx * dx + dy * dy + dz * dz > kRange * kRange) return true;
        net::send(p, channel, payload);
        return true;
    });
}
} // namespace

void deployer(GlobalPos const& pos, Json const& state) { broadcast(pos, "assembly.deployer", state); }
void arm(GlobalPos const& pos, Json const& state) { broadcast(pos, "assembly.arm", state); }

void install() {}
void uninstall() {}

} // namespace lv::feature::assembly::sync
